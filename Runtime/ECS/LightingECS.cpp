#include "ECS/LightingECS.h"
#include "ECS/TransformECS.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "RHI/RenderTarget.h"
#include "RHI/SceneView.h"
#include "RHI/DebugContext.h"
#include "Engine/GameObject.h"
#include "FrameGraph/ShadowPrepassNode.h"

using namespace Sailor;
using namespace Sailor::Tasks;

namespace
{
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
}

bool CSMLightState::Equals(const CSMLightState& rhs) const
{
	if (m_componentIndex != rhs.m_componentIndex ||
		m_shadowType != rhs.m_shadowType ||
		m_snapshot.Num() != rhs.m_snapshot.Num() ||
		!AreMatricesExactlyEqual(m_lightMatrix, rhs.m_lightMatrix))
	{
		return false;
	}

	for (uint32_t i = 0; i < m_snapshot.Num(); i++)
	{
		if (m_snapshot[i] != rhs.m_snapshot[i])
		{
			return false;
		}
	}

	return true;
}

bool CSMLightState::CanReuse(
	uint32_t componentIndex,
	RHI::EShadowType shadowType,
	const glm::mat4& lightMatrix,
	uint64_t sceneRevision) const
{
	return m_componentIndex == componentIndex &&
		m_shadowType == shadowType &&
		m_sceneRevision == sceneRevision &&
		AreMatricesExactlyEqual(m_lightMatrix, lightMatrix);
}

void LightingECS::BeginPlay()
{
	auto& driver = Sailor::RHI::Renderer::GetDriver();
	m_lightsData = driver->CreateShaderBindings();
	driver->AddSsboToShaderBindings(m_lightsData, "light", sizeof(LightingECS::LightShaderData), LightsMaxNum, 0, true);

	const auto usage = RHI::ETextureUsageBit::ColorAttachment_Bit |
		RHI::ETextureUsageBit::TextureTransferSrc_Bit |
		RHI::ETextureUsageBit::TextureTransferDst_Bit |
		RHI::ETextureUsageBit::Sampled_Bit;

	m_defaultShadowMap = driver->CreateRenderTarget(glm::ivec2(1, 1), 1, ShadowMapFormat, RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Clamp, usage);

	for (uint32_t i = 0; i < NumCascades; i++)
	{
		char csmDebugName[64];
		sprintf_s(csmDebugName, sizeof(csmDebugName), "Shadow Map, CSM: %d, Cascade: %d", i / NumCascades, i % NumCascades);

		const bool bEvsmCascade = i % NumCascades == 0;
		m_csmShadowMaps.Add(driver->CreateRenderTarget(ShadowCascadeResolutions[i % NumCascades], 1,
			bEvsmCascade ? ShadowMapFormat_Evsm : ShadowMapFormat,
			bEvsmCascade ? RHI::ETextureFiltration::Linear : RHI::ETextureFiltration::Nearest,
			RHI::ETextureClamping::Clamp,
			usage));

		driver->SetDebugName(m_csmShadowMaps[m_csmShadowMaps.Num() - 1], csmDebugName);
	}

	TVector<RHI::RHITexturePtr> shadowMaps(MaxShadowsInView);
	for (uint32_t i = 0; i < MaxShadowsInView; i++)
	{
		shadowMaps[i] = (i < m_csmShadowMaps.Num()) ? m_csmShadowMaps[i] : m_defaultShadowMap;
	}

	m_shadowMaps = Sailor::RHI::Renderer::GetDriver()->AddSamplerToShaderBindings(m_lightsData, "shadowMaps", shadowMaps, 9);

	auto shaderBindingSet = m_lightsData;
	m_lightMatrices = Sailor::RHI::Renderer::GetDriver()->AddSsboToShaderBindings(shaderBindingSet, "lightsMatrices", sizeof(glm::mat4), LightingECS::MaxShadowsInView, 6);
	m_shadowIndices = Sailor::RHI::Renderer::GetDriver()->AddSsboToShaderBindings(shaderBindingSet, "shadowIndices", sizeof(uint32_t), LightingECS::MaxShadowsInView, 7);
}

Tasks::ITaskPtr LightingECS::Tick(float deltaTime)
{
	SAILOR_PROFILE_FUNCTION();

	auto renderer = App::GetSubmodule<RHI::Renderer>();
	auto driverCommands = renderer->GetDriverCommands();
	auto cmdList = GetWorld()->GetCommandList();
	auto& binding = m_lightsData->GetOrAddShaderBinding("light");

	driverCommands->BeginDebugRegion(cmdList, "LightingECS:Update Lights", RHI::DebugContext::Color_CmdTransfer);

	TVector<LightShaderData> shaderDataBatch;
	shaderDataBatch.Reserve(64);
	size_t startIndex = 0;
	uint32_t numLights = 0;

	auto flushBatch = [&]()
	{
		if (shaderDataBatch.IsEmpty())
		{
			return;
		}

		driverCommands->UpdateShaderBinding(cmdList, binding,
			shaderDataBatch.GetData(),
			sizeof(LightingECS::LightShaderData) * shaderDataBatch.Num(),
			binding->GetBufferOffset() + sizeof(LightingECS::LightShaderData) * startIndex);
		shaderDataBatch.Clear(false);
	};

	const size_t numGpuLightSlots = GetGpuLightSlotsCount(m_components.Num());
	for (size_t index = 0; index < numGpuLightSlots; index++)
	{
		auto& data = m_components[index];
		GameObjectPtr owner = data.m_owner.StaticCast<GameObject>();
		const bool bIsUsable = data.m_bIsActive && owner;
		bool bShouldWrite = false;
		LightShaderData shaderData{};

		if (bIsUsable)
		{
			numLights = (uint32_t)index + 1;
			const auto& ownerTransform = owner->GetTransformComponent();

			if (data.m_bIsDirty || data.m_frameLastChange < ownerTransform.GetFrameLastChange())
			{
				shaderData.m_type = (uint32_t)data.m_type;
				shaderData.m_shadowType = (uint32_t)data.m_shadowType;
				shaderData.m_attenuation = data.m_attenuation;
				shaderData.m_bounds = data.m_bounds;
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
			flushBatch();
			continue;
		}

		if (!shaderDataBatch.IsEmpty() && startIndex + shaderDataBatch.Num() != index)
		{
			flushBatch();
		}

		if (shaderDataBatch.IsEmpty())
		{
			startIndex = index;
		}

		shaderDataBatch.Emplace(std::move(shaderData));
	}

	flushBatch();
	m_numLights = numLights;

	driverCommands->EndDebugRegion(cmdList);

	return nullptr;
}

void LightingECS::EndPlay()
{
	m_lightsData.Clear();
	m_csmShadowMaps.Clear();
	m_defaultShadowMap.Clear();
	m_shadowMaps.Clear();
	m_lightMatrices.Clear();
	m_shadowIndices.Clear();
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
	for (size_t index = 0; index < numGpuLightSlots; index++)
	{
		auto& light = m_components[index];
		if (light.m_shadowType != RHI::EShadowType::None && light.m_bIsActive)
		{
			GameObjectPtr owner = light.m_owner.StaticCast<GameObject>();
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
			lightProxy.m_shadowType = light.m_shadowType;

			if (light.m_type != ELightType::Directional)
			{
				const float sphereRadius = std::max(std::max(light.m_bounds.x, light.m_bounds.y), light.m_bounds.z);

				if (frustum.ContainsSphere(Math::Sphere(ownerTransform.GetPosition(), sphereRadius)))
				{
					// TODO: Sort by screen size, not by distance to camera
					lightProxy.m_distanceToCamera = glm::length(ownerTransform.GetPosition() - cameraTransform.m_position);

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

TVector<RHI::RHIUpdateShadowMapCommand> LightingECS::PrepareCSMPasses(
	const RHI::RHISceneViewPtr& sceneView,
	const Math::Transform& cameraTransform,
	const CameraData& cameraData,
	const TVector<RHI::RHILightProxy>& directionalLights,
	uint32_t& snapshotIndex)
{
	SAILOR_PROFILE_FUNCTION();

	TVector<RHI::RHIUpdateShadowMapCommand> updateShadowMaps{};
	updateShadowMaps.Reserve(directionalLights.Num() * NumCascades);

	for (const auto& directionalLight : directionalLights)
	{
		const auto lightCascadesMatrices = ShadowPrepassNode::CalculateLightProjectionForCascades(directionalLight.m_lightMatrix,
			cameraTransform.Matrix(),
			cameraData.GetAspect(),
			cameraData.GetFov(),
			cameraData.GetZNear(),
			cameraData.GetZFar());

		TVector<Math::Frustum> frustums(lightCascadesMatrices.Num());
		TVector<glm::mat4> lightMatrices(lightCascadesMatrices.Num());
		const bool bForceCustomDepthShadowUpdate = sceneView->m_bHasCustomDepthShadowCasters;
		bool bCanReuseAllCascades = !bForceCustomDepthShadowUpdate;
		for (uint32_t cascadeIndex = 0; cascadeIndex < lightCascadesMatrices.Num(); ++cascadeIndex)
		{
			lightMatrices[cascadeIndex] = lightCascadesMatrices[cascadeIndex] * directionalLight.m_lightMatrix;
			frustums[cascadeIndex].ExtractFrustumPlanes(lightMatrices[cascadeIndex]);
			const RHI::EShadowType shadowType = cascadeIndex > 0 ?
				RHI::EShadowType::PCF : directionalLight.m_shadowType;
			const uint32_t currentSnapshotIndex = snapshotIndex + cascadeIndex;
			bCanReuseAllCascades &= currentSnapshotIndex < m_csmSnapshots.Num() &&
				m_csmSnapshots[currentSnapshotIndex].CanReuse(
					directionalLight.m_index,
					shadowType,
					lightMatrices[cascadeIndex],
					sceneView->m_shadowCastersRevision);
		}

		if (bCanReuseAllCascades)
		{
			snapshotIndex += (uint32_t)lightCascadesMatrices.Num();
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
		const auto shadowCasters = sceneView->TraceShadowCasters(broadFrustum);

		RHI::EShadowType bCascadeAdded[NumCascades];
		const uint32_t alreadyPlacedPasses = (uint32_t)updateShadowMaps.Num();

		for (uint32_t k = 0; k < lightCascadesMatrices.Num(); ++k)
		{
			bCascadeAdded[k] = RHI::EShadowType::None;

			RHI::RHIUpdateShadowMapCommand cascade;
			cascade.m_shadowMap = m_csmShadowMaps[k];
			cascade.m_lightMatrix = lightMatrices[k];
			cascade.m_lighMatrixIndex = k;
			cascade.m_blurRadius = ShadowCascadeBlur[k];
			cascade.m_shadowType = k > 0 ? RHI::EShadowType::PCF : directionalLight.m_shadowType;
			cascade.m_meshList.Reserve(shadowCasters.Num());
			for (const auto& caster : shadowCasters)
			{
				if (caster && frustums[k].OverlapsAABB(caster->m_worldAabb))
				{
					cascade.m_meshList.Add(caster);
				}
			}

			CSMLightState snapshot{};
			snapshot.m_componentIndex = directionalLight.m_index;
			snapshot.m_shadowType = cascade.m_shadowType;
			snapshot.m_lightMatrix = lightMatrices[k];
			snapshot.m_sceneRevision = sceneView->m_shadowCastersRevision;
			snapshot.m_snapshot.Reserve(cascade.m_meshList.Num());

			for (const auto& caster : cascade.m_meshList)
			{
				snapshot.m_snapshot.Add({ caster->m_staticMeshEcs, caster->m_frame });
			}
			snapshot.m_snapshot.Sort([](const auto& lhs, const auto& rhs)
				{
					return lhs.m_first < rhs.m_first;
				});

			if (!bForceCustomDepthShadowUpdate &&
				snapshotIndex < m_csmSnapshots.Num() &&
				snapshot.Equals(m_csmSnapshots[snapshotIndex]))
			{
				m_csmSnapshots[snapshotIndex].m_sceneRevision = sceneView->m_shadowCastersRevision;
				++snapshotIndex;
				continue;
			}

			if (k > 0)
			{
				int32_t shift = -1;
				for (uint32_t z = 0; z < k; ++z)
				{
					if (bCascadeAdded[z] != RHI::EShadowType::None)
					{
						++shift;
					}

					if (bCascadeAdded[z] != cascade.m_shadowType)
					{
						continue;
					}

					const uint32_t removed = (uint32_t)cascade.m_meshList.RemoveAll([z, &frustums](const auto& m)
						{
							return m && frustums[z].OverlapsAABB(m->m_worldAabb);
						});

					if (removed > 0)
					{
						cascade.m_internalCommandsList.Add(alreadyPlacedPasses + shift);
					}
				}
			}

			if (snapshotIndex < m_csmSnapshots.Num())
			{
				m_csmSnapshots[snapshotIndex] = std::move(snapshot);
			}
			else
			{
				m_csmSnapshots.Emplace(std::move(snapshot));
			}

			bCascadeAdded[k] = cascade.m_shadowType;
			updateShadowMaps.Emplace(std::move(cascade));
			++snapshotIndex;
		}
	}

	return updateShadowMaps;
}

void LightingECS::FillLightingData(RHI::RHISceneViewPtr& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	uint32_t snapshotIndex = 0;

	for (uint32_t i = 0; i < sceneView->m_cameraTransforms.Num(); i++)
	{
		const auto& camera = sceneView->m_cameras[i];

		Math::Frustum frustum;
		frustum.ExtractFrustumPlanes(sceneView->m_cameraTransforms[i].Matrix(), camera.GetAspect(), camera.GetFov(), camera.GetZNear(), camera.GetZFar());

		// Sort all the lights per camera
		TVector<RHI::RHILightProxy> directionalLights;
		TVector<RHI::RHILightProxy> sortedSpotLights;
		TVector<RHI::RHILightProxy> sortedPointLights;

		GetLightsInFrustum(frustum, sceneView->m_cameraTransforms[i], directionalLights, sortedSpotLights, sortedPointLights);

		auto updateShadowMaps = PrepareCSMPasses(
			sceneView,
			sceneView->m_cameraTransforms[i],
			camera,
			directionalLights,
			snapshotIndex);
		sceneView->m_shadowMapsToUpdate.Add(std::move(updateShadowMaps));
	}

	if (m_csmSnapshots.Num() > snapshotIndex)
	{
		m_csmSnapshots.Resize(snapshotIndex);
	}

	sceneView->m_totalNumLights = m_numLights;
	sceneView->m_rhiLightsData = m_lightsData;
}

void LightingECS::GetLightProxies(TVector<Raytracing::LightProxy>& outLights) const
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

		ObjectPtr owner = light.m_owner;
		auto pOwner = owner.StaticCast<GameObject>();
		if (!pOwner)
		{
			continue;
		}

		const auto& transform = pOwner->GetTransformComponent();

		Raytracing::LightProxy lightProxy{};
		lightProxy.m_type = light.m_type;
		lightProxy.m_worldPosition = transform.GetWorldPosition();
		lightProxy.m_direction = glm::normalize(transform.GetForwardVector());
		lightProxy.m_intensity = light.m_intensity;
		lightProxy.m_attenuation = light.m_attenuation;
		lightProxy.m_bounds = light.m_bounds;
		lightProxy.m_cutOff = vec2(glm::cos(glm::radians(light.m_cutOff.x)), glm::cos(glm::radians(light.m_cutOff.y)));

		outLights.Add(lightProxy);
	}
}
