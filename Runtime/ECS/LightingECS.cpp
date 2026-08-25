#include "ECS/LightingECS.h"
#include "ECS/TransformECS.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "RHI/RenderTarget.h"
#include "RHI/SceneView.h"
#include "RHI/DebugContext.h"
#include "Engine/GameObject.h"
#include "Engine/GlobalIlluminationSettings.h"
#include "FrameGraph/ShadowPrepassNode.h"
#include "Settings/GraphicsSettings.h"

#include <array>

using namespace Sailor;
using namespace Sailor::Tasks;

namespace
{
	TSharedPtr<TVector<LightingECS::LightShaderData>> AcquireLightsSnapshot(
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

	bool AreMatricesExactlyEqual(const glm::mat4& lhs, const glm::mat4& rhs)
	{
		for (glm::length_t column = 0; column < lhs.length(); ++column)
		{
			for (glm::length_t row = 0; row < lhs[column].length(); ++row)
			{
				if (lhs[column][row] != rhs[column][row])
				{
					return false;
		}
			}
		}

		return true;
	}

	void ResolveShadowCasterUpdatePolicy(
		const TVector<RHI::RHIVisibleShadowCaster>& casters,
		bool& outContainsDynamicCasters,
		bool& outContainsAnimatedCasters)
	{
		outContainsDynamicCasters = false;
		outContainsAnimatedCasters = false;
		for (const auto& caster : casters)
		{
			outContainsDynamicCasters |=
				caster.GetMobility() == EMobilityType::Dynamic;
			outContainsAnimatedCasters |=
				caster.GetSkeletonOffset() !=
				(std::numeric_limits<uint32_t>::max)();
			if (outContainsDynamicCasters && outContainsAnimatedCasters)
			{
				return;
			}
		}
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

	float CalculateCsmShadowMapMemoryMb(
		const RHI::RHIRenderTargetPtr& shadowMap)
	{
		if (!shadowMap)
		{
			return 0.0f;
		}

		const float bytesPerPixel =
			shadowMap->GetFormat() == LightingECS::ShadowMapFormat_Evsm ?
				16.0f : 2.0f;
		const glm::ivec2 extent = shadowMap->GetExtent();
		return static_cast<float>(extent.x * extent.y) * bytesPerPixel /
			(1024.0f * 1024.0f);
	}
}

bool CSMLightState::CanReuse(
	uint32_t componentIndex,
	RHI::EShadowType shadowType,
	const glm::mat4& lightMatrix,
	uint64_t sceneRevision,
	const TSharedPtr<TVector<RHI::RHISceneVersionPtr>>& sceneVersions,
	const Math::Frustum& shadowFrustum,
	const RHI::RHISubmissionCompletionTokenPtr& currentSubmissionToken,
	uint64_t animationRevision) const
{
	if (m_componentIndex != componentIndex ||
		m_shadowType != shadowType ||
		!AreMatricesExactlyEqual(m_lightMatrix, lightMatrix) ||
		!m_submissionToken ||
		(!m_submissionToken->IsSuccessful() &&
			(!m_submissionToken->IsPending() ||
				m_submissionToken != currentSubmissionToken)) ||
		!m_payloadCompletionToken ||
		(!m_payloadCompletionToken->IsSuccessful() &&
			(!m_payloadCompletionToken->IsPending() ||
				m_submissionToken != currentSubmissionToken)))
	{
		return false;
	}
	if (m_bContainsDynamicCasters &&
		m_submissionToken != currentSubmissionToken)
	{
		return false;
	}
	if (m_bContainsAnimatedCasters &&
		m_animationRevision != animationRevision)
	{
		return false;
	}
	if (m_sceneRevision == sceneRevision)
	{
		return true;
	}
	if (!m_casterSceneVersions || !sceneVersions ||
		m_casterSceneVersions->Num() != sceneVersions->Num())
	{
		return false;
	}
	if (m_casterSceneVersions == sceneVersions)
	{
		return true;
	}

	for (size_t index = 0u; index < sceneVersions->Num(); ++index)
	{
		const auto& previous = (*m_casterSceneVersions)[index];
		const auto& current = (*sceneVersions)[index];
		if (previous == current)
		{
			continue;
		}
		if (!previous || !current ||
			current->HasShadowChangesIntersecting(*previous, shadowFrustum))
		{
			return false;
		}
	}

	return true;
}

void LightingECS::BeginPlay()
{
	m_shadowMapsMb = 0.0f;
	m_csmShadowMapsMb = 0.0f;
	m_localShadowAllocationRevision = 0ull;
	auto& driver = Sailor::RHI::Renderer::GetDriver();
	m_lightsData = driver->CreateShaderBindings();

	const auto usage = RHI::ETextureUsageBit::ColorAttachment_Bit |
		RHI::ETextureUsageBit::TextureTransferSrc_Bit |
		RHI::ETextureUsageBit::TextureTransferDst_Bit |
		RHI::ETextureUsageBit::Sampled_Bit;

	m_defaultShadowMap = driver->CreateRenderTarget(glm::ivec2(1, 1), 1, ShadowMapFormat, RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Clamp, usage);
	m_csmShadowMaps.Resize(NumCascades);
	m_shadowFlightResources.Resize((std::max)(
		1u,
		RHI::Renderer::GetDriver()->GetMaxFramesInFlight()));

	m_shadowMapOwners.Resize(MaxShadowsInView);
	m_shadowMapTextures.Resize(MaxShadowMapSamplers);
	for (uint32_t i = 0; i < MaxShadowsInView; i++)
	{
		m_shadowMapOwners[i] = InvalidShadowMapIndex;
	}
	for (uint32_t i = 0; i < MaxShadowMapSamplers; i++)
	{
		m_shadowMapTextures[i] = i < m_csmShadowMaps.Num() && m_csmShadowMaps[i] ?
			m_csmShadowMaps[i] : m_defaultShadowMap;
	}

	PublishShadowMapBindings();
}

void LightingECS::PublishShadowMapBindings()
{
	if (m_shadowMapTextures.IsEmpty())
	{
		return;
	}

	auto& driver = Sailor::RHI::Renderer::GetDriver();
	auto immutableTemplate = driver->CreateShaderBindings();
	m_shadowMaps = driver->AddSamplerToShaderBindings(
		immutableTemplate,
		"shadowMaps",
		m_shadowMapTextures,
		9u);
	immutableTemplate->RecalculateCompatibility();
	m_lightsData = std::move(immutableTemplate);
	m_bShadowMapBindingsDirty = false;
}

Tasks::ITaskPtr LightingECS::Tick(float deltaTime)
{
	SAILOR_PROFILE_FUNCTION();
	(void)deltaTime;
	bool bPublishedChanges = false;
	uint32_t numLights = 0;
	const size_t numGpuLightSlots = GetGpuLightSlotsCount(m_components.Num());
	const auto& graphicsProfile = App::GetActiveGraphicsSettings();
	for (size_t index = 0; index < numGpuLightSlots; ++index)
	{
		auto& data = m_components[index];
		if (data.m_bIsActive && data.m_owner &&
			ContributesToRealtimeLighting(data.m_globalIlluminationMode))
		{
			numLights = static_cast<uint32_t>(index) + 1u;
		}
	}
	if (m_cpuLightsData.Num() != numLights)
	{
		m_cpuLightsData.Resize(numLights);
		bPublishedChanges = true;
	}

	for (size_t index = 0; index < numLights; index++)
	{
		auto& data = m_components[index];
		GameObject* owner = data.m_owner ?
			static_cast<GameObject*>(data.m_owner.GetRawPtr()) : nullptr;
		const bool bIsUsable = data.m_bIsActive && owner &&
			ContributesToRealtimeLighting(data.m_globalIlluminationMode);
		bool bShouldWrite = false;
		LightShaderData shaderData{};

		if (bIsUsable)
		{
			const auto& ownerTransform = owner->GetTransformComponent();

			if (data.m_bIsDirty || data.m_frameLastChange < ownerTransform.GetFrameLastChange())
			{
				shaderData.m_type = (uint32_t)data.m_type;
				const RHI::EShadowType effectiveShadowType =
					!graphicsProfile.m_bSupportSoftShadows &&
					data.m_shadowType == RHI::EShadowType::EVSM ?
					RHI::EShadowType::PCF : data.m_shadowType;
				shaderData.m_shadowType = (uint32_t)effectiveShadowType;
				shaderData.m_activeCascadeCount = (std::clamp)(
					graphicsProfile.m_shadowCascadeCount,
					1u,
					NumCascades);
				shaderData.m_shadowBias = graphicsProfile.m_shadowBias;
				shaderData.m_attenuation = data.m_attenuation;
				shaderData.m_bounds = glm::vec3(data.m_radius);
				shaderData.m_intensity = data.m_intensity;
				shaderData.m_direction = glm::normalize(ownerTransform.GetForwardVector());
				shaderData.m_worldPosition = ownerTransform.GetWorldPosition();
				shaderData.m_cutOff = vec2(glm::cos(glm::radians(data.m_cutOff.x)), glm::cos(glm::radians(data.m_cutOff.y)));

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
		m_publishedLightsData = AcquireLightsSnapshot(
			m_lightsSnapshotPool,
			m_cpuLightsData);
		++m_lightingRevision;
	}

	return nullptr;
}

void LightingECS::EndPlay()
{
	m_lightsData.Clear();
	m_cpuLightsData.Clear();
	m_publishedLightsData.Clear();
	m_lightsSnapshotPool.Clear();
	m_lightingRevision = 0ull;
	m_csmShadowMaps.Clear();
	m_shadowFlightResources.Clear();
	m_shadowMapOwners.Clear();
	m_localShadowAllocations.Clear();
	m_localShadowAtlases.Clear();
	m_directionalLightsScratch.Clear();
	m_pointLightsScratch.Clear();
	m_spotLightsScratch.Clear();
	m_cascadeProjectionScratch.Clear();
	m_csmBroadCastersScratch.Clear();
	m_defaultShadowMap.Clear();
	m_shadowMaps.Clear();
	m_shadowMapTextures.Clear();
	m_writableLocalShadowAtlases.reset();
	m_bShadowMapBindingsDirty = false;
	m_numLights = 0;
	m_shadowMapsMb = 0.0f;
	m_csmShadowMapsMb = 0.0f;
	m_localShadowAllocationRevision = 0ull;
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
		if (light.m_shadowType != RHI::EShadowType::None &&
			light.m_bIsActive &&
			ContributesToRealtimeLighting(light.m_globalIlluminationMode))
		{
			GameObject* owner = light.m_owner ?
				static_cast<GameObject*>(light.m_owner.GetRawPtr()) : nullptr;
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
				!graphicsProfile.m_bSupportSoftShadows &&
				light.m_shadowType == RHI::EShadowType::EVSM ?
				RHI::EShadowType::PCF : light.m_shadowType;

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
						auto it = std::lower_bound(outSortedPointLights.begin(), outSortedPointLights.end(), lightProxy);
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

void LightingECS::PrepareCSMPasses(
	const RHI::RHISceneViewPtr& sceneView,
	const Math::Transform& cameraTransform,
	const CameraData& cameraData,
	const TVector<RHI::RHILightProxy>& directionalLights,
	uint32_t flightSlot,
	LightingShadowFlightResources& flightResources,
	uint32_t& snapshotIndex,
	TVector<RHI::RHIUpdateShadowMapCommand>& outUpdateShadowMaps)
{
	SAILOR_PROFILE_FUNCTION();

	outUpdateShadowMaps.Reserve(
		outUpdateShadowMaps.Num() + directionalLights.Num() * NumCascades);
	const auto casterSceneVersions = sceneView->GetRetainedSceneVersions();
	const auto submissionToken = sceneView->GetOrCreateSubmissionCompletionToken();
	auto& csmSnapshots = flightResources.m_csmSnapshots;
	const auto& graphicsProfile = App::GetActiveGraphicsSettings();

	for (const auto& directionalLight : directionalLights)
	{
		ShadowPrepassNode::CalculateLightProjectionForCascades(
			directionalLight.m_lightMatrix,
			cameraTransform.Matrix(),
			cameraData.GetAspect(),
			cameraData.GetFov(),
			cameraData.GetZNear(),
			cameraData.GetZFar(),
			m_cascadeProjectionScratch);
		const auto& lightCascadesMatrices = m_cascadeProjectionScratch;
		const uint32_t numCascades = static_cast<uint32_t>((std::min)(
			lightCascadesMatrices.Num(),
			static_cast<size_t>(NumCascades)));

		std::array<Math::Frustum, NumCascades> frustums{};
		std::array<glm::mat4, NumCascades> lightMatrices{};
		std::array<uint8_t, NumCascades> cascadeNeedsUpdate{};
		const bool bForceCustomDepthShadowUpdate = sceneView->m_bHasCustomDepthShadowCasters;
		bool bCanReuseAllCascades = true;
		for (uint32_t cascadeIndex = 0; cascadeIndex < numCascades; ++cascadeIndex)
		{
			lightMatrices[cascadeIndex] = lightCascadesMatrices[cascadeIndex] * directionalLight.m_lightMatrix;
			frustums[cascadeIndex].ExtractFrustumPlanes(lightMatrices[cascadeIndex]);
			const RHI::EShadowType shadowType = cascadeIndex > 0 ?
				RHI::EShadowType::PCF : directionalLight.m_shadowType;
			const uint32_t currentSnapshotIndex = snapshotIndex + cascadeIndex;
			const bool bCanReuseCascade = !bForceCustomDepthShadowUpdate &&
				currentSnapshotIndex < csmSnapshots.Num() &&
				currentSnapshotIndex < flightResources.m_csmShadowMaps.Num() &&
				flightResources.m_csmShadowMaps[currentSnapshotIndex] &&
				csmSnapshots[currentSnapshotIndex].m_shadowMap ==
					flightResources.m_csmShadowMaps[currentSnapshotIndex] &&
				csmSnapshots[currentSnapshotIndex].CanReuse(
					directionalLight.m_index,
					shadowType,
					lightMatrices[cascadeIndex],
					sceneView->m_shadowCastersRevision,
					casterSceneVersions,
					frustums[cascadeIndex],
					submissionToken,
					sceneView->m_animationRevision);
			cascadeNeedsUpdate[cascadeIndex] = bCanReuseCascade ? 0u : 1u;
			bCanReuseAllCascades &= bCanReuseCascade;
		}

		if (bCanReuseAllCascades)
		{
			for (uint32_t cascadeIndex = 0u;
				cascadeIndex < numCascades;
				++cascadeIndex)
			{
				auto& cached = csmSnapshots[snapshotIndex + cascadeIndex];
				cached.m_sceneRevision = sceneView->m_shadowCastersRevision;
				cached.m_animationRevision = sceneView->m_animationRevision;
				cached.m_casterSceneVersions = casterSceneVersions;
				if (cached.m_shadowMap && m_csmShadowMaps[cascadeIndex] != cached.m_shadowMap)
				{
					m_csmShadowMaps[cascadeIndex] = cached.m_shadowMap;
					m_shadowMapTextures[cascadeIndex] = cached.m_shadowMap;
					m_bShadowMapBindingsDirty = true;
				}
			}
			snapshotIndex += numCascades;
			continue;
		}

		const glm::mat4 broadLightMatrix = ShadowPrepassNode::CalculateLightProjectionMatrix(
			directionalLight.m_lightMatrix,
			cameraTransform.Matrix(),
			cameraData.GetAspect(),
			cameraData.GetFov(),
			cameraData.GetZNear(),
			(std::min)(cameraData.GetZFar(), ShadowMaxDistance),
			10.0f,
			glm::ivec2(0),
			ShadowCasterDepthExtension) * directionalLight.m_lightMatrix;
		Math::Frustum broadFrustum;
		broadFrustum.ExtractFrustumPlanes(broadLightMatrix);
		sceneView->TraceShadowCasters(
			broadFrustum,
			glm::vec3(cameraTransform.m_position),
			m_csmBroadCastersScratch);
		const auto& shadowCasters = m_csmBroadCastersScratch;
		std::array<Tasks::TaskPtr<TVector<RHI::RHIVisibleShadowCaster>>, NumCascades>
			cascadeCasterTasks{};
		for (uint32_t cascadeIndex = 0; cascadeIndex < numCascades; ++cascadeIndex)
		{
			if (cascadeNeedsUpdate[cascadeIndex] == 0u)
			{
				continue;
			}
			auto task = Tasks::CreateTask<TVector<RHI::RHIVisibleShadowCaster>>(
				"LightingECS:Build CSM Cascade",
				[&shadowCasters, &frustums, cascadeIndex]()
				{
					TVector<RHI::RHIVisibleShadowCaster> casters;
					casters.Reserve(shadowCasters.Num());
					for (const auto& caster : shadowCasters)
					{
						if (caster.GetSource() &&
							frustums[cascadeIndex].OverlapsAABB(caster.GetWorldBounds()))
						{
							casters.Add(caster);
						}
					}
					return casters;
				},
				EThreadType::Worker);
			task->Run();
			cascadeCasterTasks[cascadeIndex] = std::move(task);
		}

		for (uint32_t k = 0; k < numCascades; ++k)
		{
			if (cascadeNeedsUpdate[k] == 0u)
			{
				auto& cached = csmSnapshots[snapshotIndex];
				cached.m_sceneRevision = sceneView->m_shadowCastersRevision;
				cached.m_animationRevision = sceneView->m_animationRevision;
				cached.m_casterSceneVersions = casterSceneVersions;
				if (cached.m_shadowMap && m_csmShadowMaps[k] != cached.m_shadowMap)
				{
					m_csmShadowMaps[k] = cached.m_shadowMap;
					m_shadowMapTextures[k] = cached.m_shadowMap;
					m_bShadowMapBindingsDirty = true;
				}
				++snapshotIndex;
				continue;
			}

			cascadeCasterTasks[k]->Wait();
			RHI::RHIUpdateShadowMapCommand cascade;
			cascade.m_lightMatrix = lightMatrices[k];
			cascade.m_lighMatrixIndex = k;
			cascade.m_blurRadius = ShadowCascadeBlur[k];
			cascade.m_shadowType = k > 0 ? RHI::EShadowType::PCF : directionalLight.m_shadowType;
			cascade.m_meshList = std::move(cascadeCasterTasks[k]->m_result);
			CSMLightState snapshot{};
			snapshot.m_componentIndex = directionalLight.m_index;
			snapshot.m_shadowType = cascade.m_shadowType;
			snapshot.m_lightMatrix = lightMatrices[k];
			snapshot.m_sceneRevision = sceneView->m_shadowCastersRevision;
			snapshot.m_animationRevision = sceneView->m_animationRevision;
			snapshot.m_casterSceneVersions = casterSceneVersions;
			ResolveShadowCasterUpdatePolicy(
				cascade.m_meshList,
				snapshot.m_bContainsDynamicCasters,
				snapshot.m_bContainsAnimatedCasters);

			const bool bEvsmCascade =
				cascade.m_shadowType == RHI::EShadowType::EVSM;
			const glm::ivec2 shadowMapExtent(
				graphicsProfile.GetShadowCascadeResolution(k));
			const RHI::EFormat shadowMapFormat =
				GetCsmShadowMapFormat(cascade.m_shadowType);
			if (flightResources.m_csmShadowMaps.Num() <= snapshotIndex)
			{
				flightResources.m_csmShadowMaps.Resize(
					static_cast<size_t>(snapshotIndex) + 1u);
			}
			auto& writableShadowMap =
				flightResources.m_csmShadowMaps[snapshotIndex];
			if (writableShadowMap &&
				(writableShadowMap->GetFormat() != shadowMapFormat ||
					writableShadowMap->GetExtent().x != shadowMapExtent.x ||
					writableShadowMap->GetExtent().y != shadowMapExtent.y))
			{
				const auto staleShadowMap = writableShadowMap;
				const float staleMemoryMb =
					CalculateCsmShadowMapMemoryMb(staleShadowMap);
				m_csmShadowMapsMb = (std::max)(
					0.0f,
					m_csmShadowMapsMb - staleMemoryMb);
				m_shadowMapsMb = (std::max)(
					0.0f,
					m_shadowMapsMb - staleMemoryMb);
				if (m_csmShadowMaps[k] == staleShadowMap)
				{
					m_csmShadowMaps[k].Clear();
				}
				if (m_shadowMapTextures[k] == staleShadowMap)
				{
					m_shadowMapTextures[k] = m_defaultShadowMap;
					m_bShadowMapBindingsDirty = true;
				}
				writableShadowMap.Clear();
			}
			if (!writableShadowMap)
			{
				const auto usage = RHI::ETextureUsageBit::ColorAttachment_Bit |
					RHI::ETextureUsageBit::TextureTransferSrc_Bit |
					RHI::ETextureUsageBit::TextureTransferDst_Bit |
					RHI::ETextureUsageBit::Sampled_Bit;
				writableShadowMap = RHI::Renderer::GetDriver()->CreateRenderTarget(
					shadowMapExtent,
					1,
					shadowMapFormat,
					bEvsmCascade ? RHI::ETextureFiltration::Linear : RHI::ETextureFiltration::Nearest,
					RHI::ETextureClamping::Clamp,
					usage);
				if (writableShadowMap)
				{
					const float shadowMapMemoryMb =
						CalculateCsmShadowMapMemoryMb(writableShadowMap);
					m_csmShadowMapsMb += shadowMapMemoryMb;
					m_shadowMapsMb += shadowMapMemoryMb;
				}
			}
			if (!writableShadowMap)
			{
				++snapshotIndex;
				continue;
			}
			char csmDebugName[96];
			sprintf_s(
				csmDebugName,
				sizeof(csmDebugName),
				"Shadow Map, CSM Flight %u, View %u, Cascade %u",
				flightSlot,
				snapshotIndex / NumCascades,
				k);
			RHI::Renderer::GetDriver()->SetDebugName(writableShadowMap, csmDebugName);
			m_csmShadowMaps[k] = writableShadowMap;
			m_shadowMapTextures[k] = writableShadowMap;
			m_bShadowMapBindingsDirty = true;
			cascade.m_shadowMap = writableShadowMap;
			snapshot.m_shadowMap = cascade.m_shadowMap;
			snapshot.m_submissionToken = submissionToken;
			snapshot.m_payloadCompletionToken = AcquireShadowPayloadToken(
				snapshotIndex < csmSnapshots.Num() ?
					csmSnapshots[snapshotIndex].m_payloadCompletionToken :
					RHI::RHISubmissionCompletionTokenPtr{});
			cascade.m_payloadCompletionToken =
				snapshot.m_payloadCompletionToken;

			if (snapshotIndex < csmSnapshots.Num())
			{
				csmSnapshots[snapshotIndex] = std::move(snapshot);
			}
			else
			{
				csmSnapshots.Emplace(std::move(snapshot));
			}

			outUpdateShadowMaps.Emplace(std::move(cascade));
			++snapshotIndex;
		}
	}
}

void LightingECS::ReleaseLocalShadowAllocation(uint32_t componentIndex)
{
	if (componentIndex >= m_localShadowAllocations.Num())
	{
		return;
	}

	auto& allocation = m_localShadowAllocations[componentIndex];
	if (allocation.m_componentIndex != componentIndex)
	{
		return;
	}

	ReleaseLocalShadowTiles(allocation.m_atlasIndex, allocation.m_tiles);
	for (uint32_t slot : allocation.m_slots)
	{
		if (slot >= m_shadowMapOwners.Num())
		{
			continue;
		}

		m_shadowMapOwners[slot] = InvalidShadowMapIndex;
	}
	for (auto& flightResources : m_shadowFlightResources)
	{
		if (flightResources.m_localShadowSnapshots.Num() > componentIndex)
		{
			flightResources.m_localShadowSnapshots[componentIndex].Clear();
		}
	}

	allocation = {};
}

void LightingECS::ReleaseLocalShadowTiles(
	uint32_t atlasIndex,
	const TVector<glm::ivec4>& tiles)
{
	if (atlasIndex >= m_localShadowAtlases.Num() ||
		!m_localShadowAtlases[atlasIndex].m_texture)
	{
		return;
	}

	auto& occupancy = m_localShadowAtlases[atlasIndex].m_occupancy;
	for (const auto& tile : tiles)
	{
		const uint32_t firstX = static_cast<uint32_t>(tile.x) / LocalShadowMinResolution;
		const uint32_t firstY = static_cast<uint32_t>(tile.y) / LocalShadowMinResolution;
		const uint32_t cellCount = static_cast<uint32_t>(tile.z) / LocalShadowMinResolution;
		for (uint32_t y = firstY; y < firstY + cellCount; ++y)
		{
			for (uint32_t x = firstX; x < firstX + cellCount; ++x)
			{
				occupancy[y * LocalShadowAtlasCellsPerAxis + x] = 0;
			}
		}
	}
}

bool LightingECS::TryCreateLocalShadowAtlas(uint32_t& outAtlasIndex)
{
	if (m_shadowMapsMb + LocalShadowAtlasMemoryMb > m_shadowsMemoryBudgetMb + 0.001f)
	{
		return false;
	}

	uint32_t atlasIndex = static_cast<uint32_t>(m_localShadowAtlases.Num());
	for (uint32_t i = 0; i < m_localShadowAtlases.Num(); ++i)
	{
		if (!m_localShadowAtlases[i].m_texture)
		{
			atlasIndex = i;
			break;
		}
	}
	if (NumCascades + atlasIndex >= MaxShadowMapSamplers)
	{
		return false;
	}

	const auto usage = RHI::ETextureUsageBit::ColorAttachment_Bit |
		RHI::ETextureUsageBit::TextureTransferSrc_Bit |
		RHI::ETextureUsageBit::TextureTransferDst_Bit |
		RHI::ETextureUsageBit::Sampled_Bit;
	auto texture = RHI::Renderer::GetDriver()->CreateRenderTarget(
		glm::ivec2(LocalShadowAtlasResolution),
		1,
		ShadowMapFormat,
		RHI::ETextureFiltration::Nearest,
		RHI::ETextureClamping::Clamp,
		usage);
	if (!texture)
	{
		return false;
	}

	char debugName[64];
	sprintf_s(debugName, sizeof(debugName), "Shadow Map, Local Light Atlas %u", atlasIndex);
	RHI::Renderer::GetDriver()->SetDebugName(texture, debugName);

	LocalShadowAtlas atlas{};
	atlas.m_texture = texture;
	atlas.m_occupancy.Resize(
		static_cast<size_t>(LocalShadowAtlasCellsPerAxis) * LocalShadowAtlasCellsPerAxis);
	for (auto& occupied : atlas.m_occupancy)
	{
		occupied = 0;
	}
	if (atlasIndex == m_localShadowAtlases.Num())
	{
		m_localShadowAtlases.Add(std::move(atlas));
	}
	else
	{
		m_localShadowAtlases[atlasIndex] = std::move(atlas);
	}

	m_shadowMapTextures[NumCascades + atlasIndex] = texture;
	m_writableLocalShadowAtlases.set(atlasIndex);
	m_bShadowMapBindingsDirty = true;
	m_shadowMapsMb += LocalShadowAtlasMemoryMb;
	outAtlasIndex = atlasIndex;
	return true;
}

bool LightingECS::EnsureWritableLocalShadowAtlas(
	uint32_t atlasIndex,
	uint32_t flightSlot,
	LightingShadowFlightResources& flightResources)
{
	if (atlasIndex >= m_localShadowAtlases.Num() ||
		atlasIndex >= m_writableLocalShadowAtlases.size() ||
		!m_localShadowAtlases[atlasIndex].m_texture)
	{
		return false;
	}
	if (flightResources.m_localShadowAtlasTextures.Num() <= atlasIndex)
	{
		flightResources.m_localShadowAtlasTextures.Resize(
			static_cast<size_t>(atlasIndex) + 1u);
	}
	auto& writableTexture =
		flightResources.m_localShadowAtlasTextures[atlasIndex];
	if (m_writableLocalShadowAtlases.test(atlasIndex))
	{
		if (!writableTexture)
		{
			writableTexture = m_localShadowAtlases[atlasIndex].m_texture;
		}
		return writableTexture.IsValid();
	}

	if (!writableTexture)
	{
		if (m_shadowMapsMb + LocalShadowAtlasMemoryMb >
			m_shadowsMemoryBudgetMb + 0.001f)
		{
			return false;
		}
		const auto usage = RHI::ETextureUsageBit::ColorAttachment_Bit |
			RHI::ETextureUsageBit::TextureTransferSrc_Bit |
			RHI::ETextureUsageBit::TextureTransferDst_Bit |
			RHI::ETextureUsageBit::Sampled_Bit;
		writableTexture = RHI::Renderer::GetDriver()->CreateRenderTarget(
			glm::ivec2(LocalShadowAtlasResolution),
			1,
			ShadowMapFormat,
			RHI::ETextureFiltration::Nearest,
			RHI::ETextureClamping::Clamp,
			usage);
		if (writableTexture)
		{
			m_shadowMapsMb += LocalShadowAtlasMemoryMb;
		}
	}
	if (!writableTexture)
	{
		return false;
	}

	char debugName[64];
	sprintf_s(
		debugName,
		sizeof(debugName),
		"Shadow Map, Local Atlas %u, Flight %u",
		atlasIndex,
		flightSlot);
	RHI::Renderer::GetDriver()->SetDebugName(writableTexture, debugName);
	m_localShadowAtlases[atlasIndex].m_texture = writableTexture;
	m_shadowMapTextures[NumCascades + atlasIndex] = writableTexture;
	m_writableLocalShadowAtlases.set(atlasIndex);
	m_bShadowMapBindingsDirty = true;
	return true;
}

bool LightingECS::TryAllocateLocalShadowTilesInAtlas(
	uint32_t atlasIndex,
	uint32_t count,
	uint32_t resolution,
	TVector<glm::ivec4>& outTiles)
{
	outTiles.Clear(false);
	if (atlasIndex >= m_localShadowAtlases.Num() ||
		!m_localShadowAtlases[atlasIndex].m_texture)
	{
		return false;
	}

	auto& occupancy = m_localShadowAtlases[atlasIndex].m_occupancy;
	const uint32_t cellsPerTile = resolution / LocalShadowMinResolution;
	for (uint32_t tileIndex = 0; tileIndex < count; ++tileIndex)
	{
		bool bAllocated = false;
		for (uint32_t y = 0;
			y + cellsPerTile <= LocalShadowAtlasCellsPerAxis && !bAllocated;
			y += cellsPerTile)
		{
			for (uint32_t x = 0;
				x + cellsPerTile <= LocalShadowAtlasCellsPerAxis;
				x += cellsPerTile)
			{
				bool bFree = true;
				for (uint32_t cellY = y; cellY < y + cellsPerTile && bFree; ++cellY)
				{
					for (uint32_t cellX = x; cellX < x + cellsPerTile; ++cellX)
					{
						if (occupancy[cellY * LocalShadowAtlasCellsPerAxis + cellX] != 0)
						{
							bFree = false;
							break;
						}
					}
				}

				if (!bFree)
				{
					continue;
				}

				for (uint32_t cellY = y; cellY < y + cellsPerTile; ++cellY)
				{
					for (uint32_t cellX = x; cellX < x + cellsPerTile; ++cellX)
					{
						occupancy[cellY * LocalShadowAtlasCellsPerAxis + cellX] = 1;
					}
				}
				outTiles.Add(glm::ivec4(
					static_cast<int32_t>(x * LocalShadowMinResolution),
					static_cast<int32_t>(y * LocalShadowMinResolution),
					static_cast<int32_t>(resolution),
					static_cast<int32_t>(resolution)));
				bAllocated = true;
				break;
			}
		}

		if (!bAllocated)
		{
			ReleaseLocalShadowTiles(atlasIndex, outTiles);
		outTiles.Clear(false);
			return false;
		}
	}
	return true;
}

bool LightingECS::TryAllocateLocalShadowTiles(
	uint32_t count,
	uint32_t desiredResolution,
	uint32_t& outAtlasIndex,
	TVector<glm::ivec4>& outTiles)
{
	uint32_t resolution = glm::clamp(
		desiredResolution,
		LocalShadowMinResolution,
		LocalShadowAtlasResolution);
	while (resolution >= LocalShadowMinResolution)
	{
		for (uint32_t atlasIndex = 0; atlasIndex < m_localShadowAtlases.Num(); ++atlasIndex)
		{
			if (TryAllocateLocalShadowTilesInAtlas(
				atlasIndex,
				count,
				resolution,
				outTiles))
			{
				outAtlasIndex = atlasIndex;
				return true;
			}
		}

		uint32_t atlasIndex = InvalidShadowMapIndex;
		if (TryCreateLocalShadowAtlas(atlasIndex) &&
			TryAllocateLocalShadowTilesInAtlas(
				atlasIndex,
				count,
				resolution,
				outTiles))
		{
			outAtlasIndex = atlasIndex;
			return true;
		}
		resolution /= 2;
	}

	return false;
}

uint32_t LightingECS::CalculateLocalShadowResolution(
	const LightData& light,
	float distanceToCamera,
	const CameraData& cameraData,
	uint32_t viewportHeight) const
{
	const ELightShadowQuality effectiveQuality =
		Settings::ApplyShadowQualityCap(
			light.m_shadowQuality,
			App::GetActiveGraphicsSettings().m_shadowQuality);
	const uint32_t qualityLimit = GetLocalShadowResolution(effectiveQuality);
	const float safeDistance = (std::max)(distanceToCamera, 0.01f);
	const float focalLengthPixels = static_cast<float>((std::max)(viewportHeight, 1u)) /
		(2.0f * glm::tan(glm::radians(cameraData.GetFov()) * 0.5f));
	const float projectedDiameter = 2.0f * light.m_radius * focalLengthPixels / safeDistance;
	const uint32_t requestedPixels = static_cast<uint32_t>((std::max)(projectedDiameter, 1.0f));

	uint32_t resolution = LocalShadowMinResolution;
	while (resolution < requestedPixels && resolution < qualityLimit)
	{
		resolution *= 2;
	}
	return (std::min)(resolution, qualityLimit);
}

bool LightingECS::EnsureLocalShadowAllocation(
	uint32_t componentIndex,
	ELightType lightType,
	uint32_t desiredResolution,
	uint64_t frame)
{
	if (m_localShadowAllocations.Num() <= componentIndex)
	{
		m_localShadowAllocations.Resize(static_cast<size_t>(componentIndex) + 1);
	}

	auto& current = m_localShadowAllocations[componentIndex];
	if (current.m_componentIndex == componentIndex &&
		current.m_lightType == lightType &&
		!current.m_slots.IsEmpty())
	{
		if (desiredResolution < current.m_resolution)
		{
			uint32_t destinationAtlasIndex = InvalidShadowMapIndex;
			TVector<glm::ivec4> destinationTiles;
			if (TryAllocateLocalShadowTiles(
				static_cast<uint32_t>(current.m_tiles.Num()),
				desiredResolution,
				destinationAtlasIndex,
				destinationTiles))
			{
				ReleaseLocalShadowTiles(current.m_atlasIndex, current.m_tiles);
				current.m_atlasIndex = destinationAtlasIndex;
				current.m_tiles = std::move(destinationTiles);
				current.m_resolution = static_cast<uint32_t>(current.m_tiles[0].z);
				current.m_revision = ++m_localShadowAllocationRevision;
				if (current.m_revision == 0ull)
				{
					current.m_revision = ++m_localShadowAllocationRevision;
				}
			}
		}

		if (desiredResolution <= current.m_resolution)
		{
			current.m_requestedResolution = desiredResolution;
			current.m_lastUsedFrame = frame;
			return true;
		}

		// Increasing resolution requires a new shadow render. Keep the old
		// allocation alive until a replacement has been found below.
	}

	const uint32_t mapCount = GetLocalShadowMapCount(lightType);
	if (mapCount == 0)
	{
		return false;
	}

	uint32_t firstSlot = InvalidShadowMapIndex;
	if (current.m_componentIndex == componentIndex &&
		current.m_lightType == lightType &&
		current.m_slots.Num() == mapCount)
	{
		firstSlot = current.m_slots[0];
	}
	else
	{
		auto findFreeSlots = [&]()
		{
			for (uint32_t candidate = NumCascades;
				candidate + mapCount <= MaxShadowsInView;
				++candidate)
			{
				bool bRangeAvailable = true;
				for (uint32_t offset = 0; offset < mapCount; ++offset)
				{
					if (m_shadowMapOwners[candidate + offset] != InvalidShadowMapIndex)
					{
						bRangeAvailable = false;
						break;
					}
				}

				if (bRangeAvailable)
				{
					return candidate;
				}
			}
			return InvalidShadowMapIndex;
		};

		firstSlot = findFreeSlots();
		while (firstSlot == InvalidShadowMapIndex &&
			EvictLeastRecentlyUsedLocalShadowAllocation(componentIndex, frame))
		{
			firstSlot = findFreeSlots();
		}
	}

	if (firstSlot == InvalidShadowMapIndex)
	{
		if (current.m_componentIndex == componentIndex)
		{
			current.m_lastUsedFrame = frame;
			return true;
		}
		return false;
	}

	uint32_t atlasIndex = InvalidShadowMapIndex;
	TVector<glm::ivec4> tiles;
	bool bAllocatedTiles = TryAllocateLocalShadowTiles(
		mapCount,
		desiredResolution,
		atlasIndex,
		tiles);
	while (!bAllocatedTiles &&
		EvictLeastRecentlyUsedLocalShadowAllocation(componentIndex, frame))
	{
		bAllocatedTiles = TryAllocateLocalShadowTiles(
			mapCount,
			desiredResolution,
			atlasIndex,
			tiles);
	}
	if (!bAllocatedTiles)
	{
		if (current.m_componentIndex == componentIndex)
		{
			current.m_lastUsedFrame = frame;
			return true;
		}
		return false;
	}
	if (current.m_componentIndex == componentIndex &&
		current.m_lightType == lightType &&
		static_cast<uint32_t>(tiles[0].z) <= current.m_resolution)
	{
		ReleaseLocalShadowTiles(atlasIndex, tiles);
		current.m_requestedResolution = desiredResolution;
		current.m_lastUsedFrame = frame;
		return true;
	}

	LocalLightShadowAllocation allocation{};
	allocation.m_componentIndex = componentIndex;
	allocation.m_lightType = lightType;
	allocation.m_resolution = static_cast<uint32_t>(tiles[0].z);
	allocation.m_requestedResolution = desiredResolution;
	allocation.m_atlasIndex = atlasIndex;
	allocation.m_lastUsedFrame = frame;
	allocation.m_revision = ++m_localShadowAllocationRevision;
	if (allocation.m_revision == 0ull)
	{
		allocation.m_revision = ++m_localShadowAllocationRevision;
	}
	allocation.m_slots.Reserve(mapCount);
	allocation.m_tiles = std::move(tiles);

	if (current.m_componentIndex == componentIndex &&
		current.m_lightType == lightType &&
		current.m_slots.Num() == mapCount)
	{
		allocation.m_slots = current.m_slots;
		ReleaseLocalShadowTiles(current.m_atlasIndex, current.m_tiles);
	}
	else
	{
		ReleaseLocalShadowAllocation(componentIndex);
		for (uint32_t face = 0; face < mapCount; ++face)
		{
			const uint32_t slot = firstSlot + face;
			m_shadowMapOwners[slot] = componentIndex;
			allocation.m_slots.Add(slot);
		}
	}

	m_localShadowAllocations[componentIndex] = std::move(allocation);
	return true;
}

bool LightingECS::EvictLeastRecentlyUsedLocalShadowAllocation(
	uint32_t protectedComponentIndex,
	uint64_t frame)
{
	uint32_t oldestComponentIndex = InvalidShadowMapIndex;
	uint64_t oldestFrame = frame;
	for (uint32_t componentIndex = 0;
		componentIndex < m_localShadowAllocations.Num();
		++componentIndex)
	{
		const auto& allocation = m_localShadowAllocations[componentIndex];
		if (componentIndex == protectedComponentIndex ||
			allocation.m_componentIndex != componentIndex ||
			allocation.m_lastUsedFrame >= frame ||
			allocation.m_lastUsedFrame > oldestFrame)
		{
			continue;
		}

		oldestFrame = allocation.m_lastUsedFrame;
		oldestComponentIndex = componentIndex;
	}

	if (oldestComponentIndex == InvalidShadowMapIndex)
	{
		return false;
	}

	ReleaseLocalShadowAllocation(oldestComponentIndex);
	return true;
}

void LightingECS::ReleaseUnusedLocalShadowAllocations(uint64_t frame)
{
	for (uint32_t componentIndex = 0;
		componentIndex < m_localShadowAllocations.Num();
		++componentIndex)
	{
		const auto& allocation = m_localShadowAllocations[componentIndex];
		const bool bComponentCannotCastShadows =
			componentIndex >= m_components.Num() ||
			!m_components[componentIndex].m_bIsActive ||
			!ContributesToRealtimeLighting(
				m_components[componentIndex].m_globalIlluminationMode) ||
			m_components[componentIndex].m_shadowType == RHI::EShadowType::None;
		const bool bExpired = frame > allocation.m_lastUsedFrame &&
			frame - allocation.m_lastUsedFrame > LocalShadowCacheRetentionFrames;
		if (allocation.m_componentIndex == componentIndex &&
			(bComponentCannotCastShadows || bExpired))
		{
			ReleaseLocalShadowAllocation(componentIndex);
		}
	}
}

void LightingECS::ReleaseUnusedLocalShadowAtlases()
{
	for (uint32_t atlasIndex = 0; atlasIndex < m_localShadowAtlases.Num(); ++atlasIndex)
	{
		auto& atlas = m_localShadowAtlases[atlasIndex];
		if (!atlas.m_texture)
		{
			continue;
		}

		bool bUsed = false;
		for (const auto occupied : atlas.m_occupancy)
		{
			if (occupied != 0)
			{
				bUsed = true;
				break;
			}
		}
		if (bUsed)
		{
			continue;
		}

		m_shadowMapTextures[NumCascades + atlasIndex] = m_defaultShadowMap;
		m_bShadowMapBindingsDirty = true;
		const auto latestTexture = atlas.m_texture;
		bool bLatestOwnedByFlight = false;
		size_t numPhysicalTextures = 0u;
		for (auto& flightResources : m_shadowFlightResources)
		{
			if (flightResources.m_localShadowAtlasTextures.Num() <= atlasIndex)
			{
				continue;
			}
			auto& flightTexture =
				flightResources.m_localShadowAtlasTextures[atlasIndex];
			if (!flightTexture)
			{
				continue;
			}
			bLatestOwnedByFlight |= flightTexture == latestTexture;
			++numPhysicalTextures;
			flightTexture.Clear();
		}
		if (latestTexture && !bLatestOwnedByFlight)
		{
			++numPhysicalTextures;
		}
		atlas = {};
		m_shadowMapsMb = (std::max)(
			0.0f,
			m_shadowMapsMb -
				LocalShadowAtlasMemoryMb * static_cast<float>(numPhysicalTextures));
	}
}

void LightingECS::PrepareLocalShadowPasses(
	const RHI::RHISceneViewPtr& sceneView,
	const TVector<RHI::RHILightProxy>& spotLights,
	const TVector<RHI::RHILightProxy>& pointLights,
	const Math::Transform& cameraTransform,
	const CameraData& cameraData,
	uint32_t viewportHeight,
	uint32_t flightSlot,
	LightingShadowFlightResources& flightResources,
	TVector<uint32_t>& shadowIndices,
	TVector<uint32_t>& shadowAtlasTiles,
	TVector<RHI::RHIUpdateShadowMapCommand>& outUpdateShadowMaps)
{
	const uint64_t frame = GetWorld()->GetCurrentFrame();
	const auto casterSceneVersions = sceneView->GetRetainedSceneVersions();
	const auto submissionToken = sceneView->GetOrCreateSubmissionCompletionToken();
	outUpdateShadowMaps.Reserve(
		outUpdateShadowMaps.Num() +
		spotLights.Num() + pointLights.Num() * 6u);

	auto prepareLights = [&](const TVector<RHI::RHILightProxy>& lights)
	{
		for (const auto& lightProxy : lights)
		{
			if (lightProxy.m_index >= m_components.Num() ||
				lightProxy.m_index >= shadowIndices.Num())
			{
				continue;
			}

			const auto& light = m_components[lightProxy.m_index];
			const uint32_t desiredResolution = CalculateLocalShadowResolution(
				light,
				lightProxy.m_distanceToCamera,
				cameraData,
				viewportHeight);
			if (light.m_shadowType == RHI::EShadowType::None ||
				(light.m_type != ELightType::Point && light.m_type != ELightType::Spot) ||
				!EnsureLocalShadowAllocation(
					lightProxy.m_index,
					light.m_type,
					desiredResolution,
					frame))
			{
				continue;
			}

			auto& allocation = m_localShadowAllocations[lightProxy.m_index];
			GameObject* owner = light.m_owner ?
				static_cast<GameObject*>(light.m_owner.GetRawPtr()) : nullptr;
			if (!owner)
			{
				continue;
			}

			const auto& transform = owner->GetTransformComponent();
			const glm::vec3 position = transform.GetWorldPosition();
			const float farPlane = (std::max)(light.m_radius, 0.01f);
			if (farPlane <= 0.05f)
			{
				shadowIndices[lightProxy.m_index] = InvalidShadowMapIndex;
				continue;
			}
			if (!EnsureWritableLocalShadowAtlas(
					allocation.m_atlasIndex,
					flightSlot,
					flightResources))
			{
				continue;
			}
			if (flightResources.m_localShadowSnapshots.Num() <= lightProxy.m_index)
			{
				flightResources.m_localShadowSnapshots.Resize(
					static_cast<size_t>(lightProxy.m_index) + 1u);
			}
			auto& flightSnapshots =
				flightResources.m_localShadowSnapshots[lightProxy.m_index];
			const uint32_t mapCount = GetLocalShadowMapCount(light.m_type);
			if (flightSnapshots.Num() != mapCount)
			{
				flightSnapshots.Resize(mapCount);
			}
			const uint32_t firstSlot = allocation.m_slots[0];
			shadowIndices[lightProxy.m_index] = firstSlot |
				(light.m_shadowFilter == ELightShadowFilter::Soft &&
					App::GetActiveGraphicsSettings().m_bSupportSoftShadows ?
					SoftShadowMapBit : 0u);

			const float nearPlane = (std::max)(0.01f, farPlane * 0.001f);
			std::array<glm::mat4, 6u> lightMatrices{};
			uint32_t numLightMatrices = 0u;
			if (light.m_type == ELightType::Spot)
			{
				const float fieldOfView = glm::clamp(
					glm::abs(light.m_cutOff.y) * 2.0f,
					1.0f,
					179.0f);
				const glm::mat4 view = glm::lookAtRH(
					position,
					position + glm::normalize(transform.GetForwardVector()),
					glm::normalize(glm::vec3(
						transform.GetCachedWorldMatrix() * Math::vec4_Up)));
				lightMatrices[0] = Math::PerspectiveRH(
					glm::radians(fieldOfView), 1.0f, nearPlane, farPlane) * view;
				numLightMatrices = 1u;
			}
			else
			{
				static constexpr glm::vec3 directions[6] = {
					{ 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f },
					{ 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f },
					{ 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f }
				};
				static constexpr glm::vec3 upVectors[6] = {
					{ 0.0f, -1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f },
					{ 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f },
					{ 0.0f, -1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }
				};
				const glm::mat4 projection = Math::PerspectiveRH(
					glm::radians(90.0f), 1.0f, nearPlane, farPlane);
				for (uint32_t face = 0; face < 6; ++face)
				{
					lightMatrices[face] = projection * glm::lookAtRH(
						position,
						position + directions[face],
						upVectors[face]);
				}
				numLightMatrices = 6u;
			}

			for (uint32_t face = 0; face < numLightMatrices; ++face)
			{
				const uint32_t shadowSlot = allocation.m_slots[face];
				if (shadowAtlasTiles.Num() <= shadowSlot)
				{
					shadowAtlasTiles.Resize(static_cast<size_t>(shadowSlot) + 1u);
				}
				const glm::ivec4 tile = allocation.m_tiles[face];
				const uint32_t tileX = static_cast<uint32_t>(tile.x) / LocalShadowMinResolution;
				const uint32_t tileY = static_cast<uint32_t>(tile.y) / LocalShadowMinResolution;
				const uint32_t tileCells = static_cast<uint32_t>(tile.z) / LocalShadowMinResolution;
				const uint32_t tileLevel = static_cast<uint32_t>(glm::log2(static_cast<float>(tileCells)));
				shadowAtlasTiles[shadowSlot] = tileX |
					(tileY << 6u) |
					(tileLevel << 12u) |
					(allocation.m_atlasIndex << 15u);
				auto& cachedState = flightSnapshots[face];
				Math::Frustum shadowFrustum(lightMatrices[face]);
				if (cachedState.m_resourceRevision == allocation.m_revision &&
					cachedState.CanReuse(
						lightProxy.m_index,
						RHI::EShadowType::PCF,
						lightMatrices[face],
						sceneView->m_shadowCastersRevision,
						casterSceneVersions,
						shadowFrustum,
						submissionToken,
						sceneView->m_animationRevision))
				{
					cachedState.m_sceneRevision = sceneView->m_shadowCastersRevision;
					cachedState.m_animationRevision = sceneView->m_animationRevision;
					cachedState.m_casterSceneVersions = casterSceneVersions;
					continue;
				}

				RHI::RHIUpdateShadowMapCommand shadowPass{};
				shadowPass.m_renderArea = tile;
				shadowPass.m_lightMatrix = lightMatrices[face];
				shadowPass.m_lighMatrixIndex = shadowSlot;
				shadowPass.m_shadowType = RHI::EShadowType::PCF;
				shadowPass.m_meshList = sceneView->TraceShadowCasters(
					shadowFrustum,
					glm::vec3(cameraTransform.m_position));

				CSMLightState snapshot{};
				snapshot.m_componentIndex = lightProxy.m_index;
				snapshot.m_shadowType = RHI::EShadowType::PCF;
				snapshot.m_lightMatrix = lightMatrices[face];
				snapshot.m_sceneRevision = sceneView->m_shadowCastersRevision;
				snapshot.m_animationRevision = sceneView->m_animationRevision;
				snapshot.m_resourceRevision = allocation.m_revision;
				snapshot.m_casterSceneVersions = casterSceneVersions;
				ResolveShadowCasterUpdatePolicy(
					shadowPass.m_meshList,
					snapshot.m_bContainsDynamicCasters,
					snapshot.m_bContainsAnimatedCasters);
				shadowPass.m_shadowMap =
					m_localShadowAtlases[allocation.m_atlasIndex].m_texture;
				snapshot.m_submissionToken = submissionToken;
				snapshot.m_payloadCompletionToken = AcquireShadowPayloadToken(
					cachedState.m_payloadCompletionToken);
				shadowPass.m_payloadCompletionToken =
					snapshot.m_payloadCompletionToken;
				cachedState = std::move(snapshot);
				outUpdateShadowMaps.Emplace(std::move(shadowPass));
			}
		}
	};

	prepareLights(spotLights);
	prepareLights(pointLights);
}

void LightingECS::FillLightingData(RHI::RHISceneViewPtr& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	if (!sceneView || !sceneView->m_submissionContext)
	{
		SAILOR_LOG_ERROR(
			"LightingECS::FillLightingData requires an acquired render submission flight.");
		return;
	}
	const uint32_t flightSlot =
		sceneView->m_submissionContext->GetFlightSlot();
	if (m_shadowFlightResources.Num() <= flightSlot)
	{
		m_shadowFlightResources.Resize(static_cast<size_t>(flightSlot) + 1u);
	}
	auto& flightResources = m_shadowFlightResources[flightSlot];
	m_writableLocalShadowAtlases.reset();
	uint32_t snapshotIndex = 0;
	const glm::ivec2 viewportExtent = App::GetMainWindow()->GetRenderArea();
	const Settings::GraphicsExtent renderExtent =
		Settings::ResolveRenderDimensions(
			static_cast<uint32_t>((std::max)(viewportExtent.x, 1)),
			static_cast<uint32_t>((std::max)(viewportExtent.y, 1)),
			App::GetActiveGraphicsSettings().m_resolutionFactor);
	const uint32_t viewportHeight = renderExtent.m_height;
	const size_t numCameras = sceneView->m_cameraTransforms.Num();
	sceneView->m_shadowMapsToUpdate.Resize(numCameras);
	sceneView->m_shadowMapsToBlit.Resize(numCameras);
	sceneView->m_shadowIndices.Resize(numCameras);
	sceneView->m_shadowAtlasTiles.Resize(numCameras);
	sceneView->m_shadowMatrices.Resize(numCameras);

	for (uint32_t i = 0; i < numCameras; i++)
	{
		auto& updateShadowMaps = sceneView->m_shadowMapsToUpdate[i];
		auto& shadowMapsToBlit = sceneView->m_shadowMapsToBlit[i];
		auto& shadowIndices = sceneView->m_shadowIndices[i];
		auto& shadowAtlasTiles = sceneView->m_shadowAtlasTiles[i];
		auto& shadowMatrices = sceneView->m_shadowMatrices[i];
		if (i < sceneView->m_snapshots.Num())
		{
			auto& previous = sceneView->m_snapshots[i];
			updateShadowMaps = std::move(previous.m_shadowMapsToUpdate);
			shadowMapsToBlit = std::move(previous.m_shadowMapsToBlit);
			shadowIndices = std::move(previous.m_shadowIndices);
			shadowAtlasTiles = std::move(previous.m_shadowAtlasTiles);
			shadowMatrices = std::move(previous.m_shadowMatrices);
		}
		updateShadowMaps.Clear(false);
		shadowMapsToBlit.Clear(false);
		shadowIndices.Resize(m_numLights);
		shadowAtlasTiles.Resize(NumCascades);
		shadowMatrices.Resize(NumCascades);
		for (auto& shadowIndex : shadowIndices)
		{
			shadowIndex = InvalidShadowMapIndex;
		}
		for (auto& atlasTile : shadowAtlasTiles)
		{
			atlasTile = 0u;
		}
		for (auto& matrix : shadowMatrices)
		{
			matrix = glm::mat4(1.0f);
		}

		const auto& camera = sceneView->m_cameras[i];

		Math::Frustum frustum;
		frustum.ExtractFrustumPlanes(sceneView->m_cameraTransforms[i].Matrix(), camera.GetAspect(), camera.GetFov(), camera.GetZNear(), camera.GetZFar());

		// Sort all the lights per camera
		m_directionalLightsScratch.Clear(false);
		m_pointLightsScratch.Clear(false);
		m_spotLightsScratch.Clear(false);

		GetLightsInFrustum(
			frustum,
			sceneView->m_cameraTransforms[i],
			m_directionalLightsScratch,
			m_pointLightsScratch,
			m_spotLightsScratch);

		const uint32_t cameraCsmSnapshotStart = snapshotIndex;
		PrepareCSMPasses(
			sceneView,
			sceneView->m_cameraTransforms[i],
			camera,
			m_directionalLightsScratch,
			flightSlot,
			flightResources,
			snapshotIndex,
			updateShadowMaps);
		PrepareLocalShadowPasses(
			sceneView,
			m_spotLightsScratch,
			m_pointLightsScratch,
			sceneView->m_cameraTransforms[i],
			camera,
			viewportHeight,
			flightSlot,
			flightResources,
			shadowIndices,
			shadowAtlasTiles,
			updateShadowMaps);
		for (uint32_t cascadeIndex = 0u;
			cascadeIndex < NumCascades &&
			cameraCsmSnapshotStart + cascadeIndex <
				flightResources.m_csmSnapshots.Num();
			++cascadeIndex)
		{
			shadowMatrices[cascadeIndex] =
				flightResources.m_csmSnapshots[
					cameraCsmSnapshotStart + cascadeIndex].m_lightMatrix;
		}
		for (const auto& allocation : m_localShadowAllocations)
		{
			if (allocation.m_componentIndex == InvalidShadowMapIndex ||
				allocation.m_componentIndex >= shadowIndices.Num() ||
				shadowIndices[allocation.m_componentIndex] == InvalidShadowMapIndex)
			{
				continue;
			}
			if (flightResources.m_localShadowSnapshots.Num() <=
				allocation.m_componentIndex)
			{
				continue;
			}
			const auto& flightSnapshots =
				flightResources.m_localShadowSnapshots[
					allocation.m_componentIndex];
			const uint32_t numFaces = static_cast<uint32_t>((std::min)(
				allocation.m_slots.Num(),
				flightSnapshots.Num()));
			for (uint32_t face = 0u; face < numFaces; ++face)
			{
				const uint32_t slot = allocation.m_slots[face];
				if (shadowMatrices.Num() <= slot)
				{
					const size_t previousSize = shadowMatrices.Num();
					shadowMatrices.Resize(static_cast<size_t>(slot) + 1u);
					for (size_t matrixIndex = previousSize;
						matrixIndex < shadowMatrices.Num(); ++matrixIndex)
					{
						shadowMatrices[matrixIndex] = glm::mat4(1.0f);
					}
				}
				shadowMatrices[slot] = flightSnapshots[face].m_lightMatrix;
			}
		}
		if (m_bShadowMapBindingsDirty)
		{
			PublishShadowMapBindings();
		}
		sceneView->m_rhiLightsDataPerCamera.Add(m_lightsData);
	}

	if (flightResources.m_csmSnapshots.Num() > snapshotIndex)
	{
		flightResources.m_csmSnapshots.Resize(snapshotIndex);
	}

	ReleaseUnusedLocalShadowAllocations(GetWorld()->GetCurrentFrame());
	ReleaseUnusedLocalShadowAtlases();
	if (m_bShadowMapBindingsDirty)
	{
		PublishShadowMapBindings();
	}

	sceneView->m_totalNumLights = m_numLights;
	sceneView->m_rhiLightsData = m_lightsData;
	sceneView->m_cpuLightsData = m_publishedLightsData;
	sceneView->m_lightingRevision = m_lightingRevision;
}

void LightingECS::GetLightProxies(TVector<Raytracing::LightProxy>& outLights) const
{
	CollectLightProxies(outLights, false);
}

void LightingECS::GetGlobalIlluminationBakeLightProxies(
	TVector<Raytracing::LightProxy>& outLights) const
{
	CollectLightProxies(outLights, true);
}

void LightingECS::CollectLightProxies(
	TVector<Raytracing::LightProxy>& outLights,
	bool bGlobalIlluminationBakeContributorsOnly) const
{
	outLights.Clear();
	const size_t numGpuLightSlots = GetGpuLightSlotsCount(m_components.Num());
	outLights.Reserve(numGpuLightSlots);

	for (size_t index = 0; index < numGpuLightSlots; ++index)
	{
		const auto& light = m_components[index];
		if (!light.m_bIsActive)
		{
			continue;
		}
		const bool bContributes = bGlobalIlluminationBakeContributorsOnly ?
			ContributesToBakedGlobalIllumination(
				light.m_globalIlluminationMode) :
			ContributesToRealtimeLighting(light.m_globalIlluminationMode);
		if (!bContributes)
		{
			continue;
		}

		GameObject* pOwner = light.m_owner ?
			static_cast<GameObject*>(light.m_owner.GetRawPtr()) : nullptr;
		if (!pOwner)
		{
			continue;
		}
		if (bGlobalIlluminationBakeContributorsOnly &&
			!IsGlobalIlluminationBakeContributor(pOwner->GetMobilityType()))
		{
			continue;
		}

		const auto& transform = pOwner->GetTransformComponent();

		Raytracing::LightProxy lightProxy{};
		lightProxy.m_type = light.m_type;
		lightProxy.m_worldPosition = transform.GetWorldPosition();
		lightProxy.m_direction = glm::normalize(transform.GetForwardVector());
		lightProxy.m_intensity = light.m_intensity;
		lightProxy.m_indirectLightingIntensity =
			light.m_indirectLightingIntensity;
		lightProxy.m_attenuation = light.m_attenuation;
		lightProxy.m_bounds = glm::vec3(light.m_radius);
		lightProxy.m_cutOff = vec2(glm::cos(glm::radians(light.m_cutOff.x)), glm::cos(glm::radians(light.m_cutOff.y)));

		outLights.Add(lightProxy);
	}
}
