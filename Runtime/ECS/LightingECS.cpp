#include "ECS/LightingECS.h"
#include "ECS/LightingECSInternal.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Components/SkyComponent.h"
#include "GlobalIllumination/GISettings.h"
#include "RHI/DebugContext.h"
#include "RHI/RenderTarget.h"
#include "RHI/SceneView.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "Settings/GraphicsSettings.h"

#include <algorithm>
#include <limits>
#include <utility>

using namespace Sailor;
using namespace Sailor::LightingECSInternal;
using namespace Sailor::Tasks;

namespace Sailor::LightingECSInternal
{
	TSharedPtr<const TVector<LightingECS::LightShaderData>> AcquireLightsSnapshot(
		TVector<TSharedPtr<TVector<LightingECS::LightShaderData>>>& pool,
		const TVector<LightingECS::LightShaderData>& source)
	{
		for (auto& candidate : pool)
		{
			if (candidate && !candidate.IsShared())
			{
				*candidate = source;
				return candidate;
			}
		}

		auto snapshot = TSharedPtr<TVector<LightingECS::LightShaderData>>::Make();
		*snapshot = source;
		pool.Add(snapshot);
		return snapshot;
	}

	void ResolveShadowCasterUpdatePolicy(const TVector<RHI::RHIVisibleShadowCaster>& casters,
		bool& outContainsDynamicCasters,
		bool& outContainsAnimatedCasters,
		bool& outContainsCameraLodCasters)
	{
		outContainsDynamicCasters = false;
		outContainsAnimatedCasters = false;
		outContainsCameraLodCasters = false;
		for (const auto& caster : casters)
		{
			outContainsDynamicCasters |= caster.GetMobility() == EMobilityType::Dynamic;
			outContainsAnimatedCasters |= caster.GetSkeletonOffset() != (std::numeric_limits<uint32_t>::max)();
			outContainsCameraLodCasters |= caster.m_resource && caster.m_resource->m_proxy.m_lodPolicy.m_bEnabled;
			if (outContainsDynamicCasters && outContainsAnimatedCasters && outContainsCameraLodCasters)
			{
				return;
			}
		}
	}

	size_t CalculateShadowLodCameraRevision(const CameraData& camera)
	{
		size_t revision = 0u;
		HashCombine(revision, std::hash<glm::mat4>{}(camera.GetViewMatrix()),
			std::hash<glm::mat4>{}(camera.GetProjectionMatrix()), App::GetActiveGraphicsSettings().m_lodBias);
		return revision;
	}

	RHI::RHISubmissionCompletionTokenPtr AcquireShadowPayloadToken(
		const RHI::RHISubmissionCompletionTokenPtr& cachedToken)
	{
		auto token = cachedToken;
		if (token)
		{
			token->Reset();
		}
		else
		{
			token = RHI::RHISubmissionCompletionTokenPtr::Make();
		}
		return token;
	}

	float CalculateCsmShadowMapMemoryMb(const RHI::RHIRenderTargetPtr& shadowMap)
	{
		if (!shadowMap)
		{
			return 0.0f;
		}

		const float bytesPerPixel = shadowMap->GetFormat() == LightingECS::ShadowMapFormat_Evsm ? 16.0f : 2.0f;
		const glm::ivec2 extent = shadowMap->GetExtent();
		return static_cast<float>(extent.x * extent.y) * bytesPerPixel / (1024.0f * 1024.0f);
	}
}

void LightingECS::RegisterSky(TObjectPtr<SkyComponent> sky)
{
	m_skies.Add(std::move(sky));
}

void LightingECS::UnregisterSky(const SkyComponent* sky)
{
	for (size_t i = 0; i < m_skies.Num(); ++i)
	{
		if (m_skies[i].GetRawPtr() == sky)
		{
			m_skies.RemoveAt(i);
			break;
		}
	}
}

TObjectPtr<SkyComponent> LightingECS::GetSky() const
{
	TObjectPtr<SkyComponent> selected;
	for (const auto& sky : m_skies)
	{
		if (!static_cast<bool>(*sky->GetOwner())) continue;
		if (!selected || sky->GetOwner()->GetInstanceId().ToString() < selected->GetOwner()->GetInstanceId().ToString())
		{
			selected = sky;
		}
	}
	return selected;
}

void LightingECS::BeginPlay()
{
	m_shadows.m_mapsMb = 0.0f;
	m_shadows.m_csmMapsMb = 0.0f;
	m_shadows.m_localAllocationRevision = 0ull;
	auto& driver = Sailor::RHI::Renderer::GetDriver();
	m_shadows.m_bindings = driver->CreateShaderBindings();

	m_shadows.m_csmMaps.Resize(NumCascades);
	m_shadows.m_flights.Resize((std::max)(1u, RHI::Renderer::GetDriver()->GetMaxFramesInFlight()));

	m_shadows.m_mapOwners.Resize(MaxShadowsInView);
	m_shadows.m_textures.Resize(MaxShadowMapSamplers);
	for (uint32_t i = 0; i < MaxShadowsInView; i++)
	{
		m_shadows.m_mapOwners[i] = InvalidShadowMapIndex;
	}
	for (uint32_t i = 0; i < MaxShadowMapSamplers; i++)
	{
		m_shadows.m_textures[i] =
			i < m_shadows.m_csmMaps.Num() && m_shadows.m_csmMaps[i] ? m_shadows.m_csmMaps[i] : m_shadows.m_defaultMap;
	}

	PublishShadowMapBindings();
}

void LightingECS::PublishShadowMapBindings()
{
	if (m_shadows.m_textures.IsEmpty())
	{
		return;
	}

	auto& driver = Sailor::RHI::Renderer::GetDriver();
	if (!m_shadows.m_defaultMap)
	{
		const auto usage = RHI::ETextureUsageBit::ColorAttachment_Bit | RHI::ETextureUsageBit::TextureTransferSrc_Bit |
			RHI::ETextureUsageBit::TextureTransferDst_Bit | RHI::ETextureUsageBit::Sampled_Bit;
		m_shadows.m_defaultMap = driver->CreateRenderTarget(glm::ivec2(1, 1), 1, ShadowMapFormat,
			RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Clamp, usage);
		if (!m_shadows.m_defaultMap)
		{
			m_shadows.m_bBindingsDirty = true;
			return;
		}
		for (auto& texture : m_shadows.m_textures)
		{
			if (!texture) texture = m_shadows.m_defaultMap;
		}
	}
	auto immutableTemplate = driver->CreateShaderBindings();
	auto shadowMaps = driver->AddSamplerToShaderBindings(immutableTemplate, "shadowMaps"_h, m_shadows.m_textures, 9u);
	if (!shadowMaps)
	{
		m_shadows.m_bBindingsDirty = true;
		return;
	}
	immutableTemplate->RecalculateCompatibility();
	m_shadows.m_bindings = std::move(immutableTemplate);
	m_shadows.m_bBindingsDirty = false;
}

void LightingECS::Tick(float deltaTime)
{
	SAILOR_PROFILE_FUNCTION();
	(void)deltaTime;
	bool bPublishedChanges = false;
	uint32_t numLights = 0;
	uint32_t directionalShadowLight = InvalidShadowMapIndex;
	const size_t numGpuLightSlots = GetGpuLightSlotsCount(m_components.Num());
	const auto& graphicsProfile = App::GetActiveGraphicsSettings();
	for (size_t index = 0; index < numGpuLightSlots; ++index)
	{
		auto& data = m_components[index];
		if (data.m_bIsActive && data.m_owner && ContributesToRealtimeLighting(data.m_globalIlluminationMode))
		{
			numLights = static_cast<uint32_t>(index) + 1u;
			if (directionalShadowLight == InvalidShadowMapIndex && data.m_type == ELightType::Directional &&
				data.m_shadowType != RHI::EShadowType::None)
			{
				directionalShadowLight = static_cast<uint32_t>(index);
			}
		}
	}
	// The GPU layout has one CSM set per camera. The first eligible light owns it.
	m_shadows.m_directionalLightIndex = directionalShadowLight;
	if (m_cpuLightsData.Num() != numLights)
	{
		m_cpuLightsData.Resize(numLights);
		bPublishedChanges = true;
	}

	for (size_t index = 0; index < numLights; index++)
	{
		auto& data = m_components[index];
		GameObject* owner = data.m_owner ? static_cast<GameObject*>(data.m_owner.GetRawPtr()) : nullptr;
		const bool bIsUsable =
			data.m_bIsActive && owner && ContributesToRealtimeLighting(data.m_globalIlluminationMode);
		bool bShouldWrite = false;
		LightShaderData shaderData{};

		if (bIsUsable)
		{
			const auto& ownerTransform = owner->GetTransformComponent();
			RHI::EShadowType effectiveShadowType =
				!graphicsProfile.m_bSupportSoftShadows && data.m_shadowType == RHI::EShadowType::EVSM
					? RHI::EShadowType::PCF : data.m_shadowType;
			if (data.m_type == ELightType::Directional && index != directionalShadowLight)
			{
				effectiveShadowType = RHI::EShadowType::None;
			}

			if (data.m_bIsDirty || data.m_frameLastChange < ownerTransform.GetFrameLastChange() ||
				m_cpuLightsData[index].m_shadowType != static_cast<uint32_t>(effectiveShadowType))
			{
				shaderData.m_type = (uint32_t)data.m_type;
				shaderData.m_shadowType = (uint32_t)effectiveShadowType;
				shaderData.m_activeCascadeCount = (std::clamp)(graphicsProfile.m_shadowCascadeCount, 1u, NumCascades);
				shaderData.m_shadowBias = graphicsProfile.m_shadowBias;
				shaderData.m_shadowDistance = graphicsProfile.m_shadowDistance;
				shaderData.m_bounds = glm::vec3(data.m_radius);
				shaderData.m_intensity = data.m_intensity;
				shaderData.m_direction = glm::normalize(ownerTransform.GetForwardVector());
				shaderData.m_worldPosition = ownerTransform.GetWorldPosition();
				shaderData.m_cutOff =
					vec2(glm::cos(glm::radians(data.m_cutOff.x)), glm::cos(glm::radians(data.m_cutOff.y)));

				data.m_frameLastChange = ownerTransform.GetFrameLastChange();
				data.m_bIsDirty = false;
				bShouldWrite = true;
			}
		}
		else if (data.m_bIsDirty)
		{
			// A released slot can remain below the highest active index. Clear its
			// previous GPU data once so the light cannot survive component removal.
			data.m_bIsDirty = false;
			bShouldWrite = true;
		}

		if (!bShouldWrite)
		{
			continue;
		}
		m_cpuLightsData[index] = std::move(shaderData);
		bPublishedChanges = true;
	}

	m_numLights = numLights;
	if (bPublishedChanges || !m_publishedLightsData)
	{
		m_publishedLightsData = AcquireLightsSnapshot(m_lightsSnapshotPool, m_cpuLightsData);
		++m_lightingRevision;
	}
}

void LightingECS::EndPlay()
{
	m_skies.Clear();
	m_cpuLightsData.Clear();
	m_publishedLightsData.Clear();
	m_lightsSnapshotPool.Clear();
	m_lightingRevision = 0ull;
	m_shadows = {};
	m_numLights = 0;
}

void LightingECS::GetLightsInFrustum(const Math::Frustum& frustum,
	const Math::Transform& cameraTransform,
	TVector<RHI::RHILightProxy>& outDirectionalLights,
	TVector<RHI::RHILightProxy>& outSortedPointLights,
	TVector<RHI::RHILightProxy>& outSortedSpotLights)
{
	SAILOR_PROFILE_FUNCTION();

	// TODO: Cache lights that cast shadows separately to decrease algo complexity
	const size_t numGpuLightSlots = GetGpuLightSlotsCount(m_components.Num());
	const auto& graphicsProfile = App::GetActiveGraphicsSettings();
	for (size_t index = 0; index < numGpuLightSlots; index++)
	{
		auto& light = m_components[index];
		if (light.m_shadowType != RHI::EShadowType::None && light.m_bIsActive &&
			ContributesToRealtimeLighting(light.m_globalIlluminationMode))
		{
			if (light.m_type == ELightType::Directional && index != m_shadows.m_directionalLightIndex)
			{
				continue;
			}
			GameObject* owner = light.m_owner ? static_cast<GameObject*>(light.m_owner.GetRawPtr()) : nullptr;
			if (!owner)
			{
				continue;
			}

			const auto& ownerTransform = owner->GetTransformComponent();

			RHI::RHILightProxy lightProxy{};

			lightProxy.m_lightMatrix = glm::inverse(ownerTransform.GetCachedWorldMatrix());
			lightProxy.m_lightTransform = ownerTransform.GetTransform();
			lightProxy.m_distanceToCamera = 0.0f;
			lightProxy.m_index = (uint32_t)index;
			lightProxy.m_shadowType =
				!graphicsProfile.m_bSupportSoftShadows && light.m_shadowType == RHI::EShadowType::EVSM
					? RHI::EShadowType::PCF
					: light.m_shadowType;

			if (light.m_type != ELightType::Directional)
			{
				const float sphereRadius = (std::max)(light.m_radius, 0.01f);
				const glm::vec3 worldPosition = ownerTransform.GetWorldPosition();

				if (frustum.OverlapsSphere(Math::Sphere(worldPosition, sphereRadius)))
				{
					// TODO: Sort by screen size, not by distance to camera
					lightProxy.m_distanceToCamera = glm::length(worldPosition - glm::vec3(cameraTransform.m_position));

					if (ELightType::Spot == light.m_type)
					{
						auto it = std::lower_bound(outSortedSpotLights.begin(), outSortedSpotLights.end(), lightProxy);
						outSortedSpotLights.Insert(lightProxy, it - outSortedSpotLights.begin());
					}
					else
					{
						auto it =
							std::lower_bound(outSortedPointLights.begin(), outSortedPointLights.end(), lightProxy);
						outSortedPointLights.Insert(lightProxy, it - outSortedPointLights.begin());
					}
				}
			}
			else
			{
				outDirectionalLights.Add(lightProxy);
			}
		}
	}
}
