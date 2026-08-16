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
		const float bytesPerPixel = bEvsmCascade ? 16.0f : 2.0f;
		m_shadowMapsMb += static_cast<float>(
			ShadowCascadeResolutions[i].x * ShadowCascadeResolutions[i].y) *
			bytesPerPixel / (1024.0f * 1024.0f);
	}

	m_shadowMapSlots.Resize(MaxShadowsInView);
	m_shadowMapOwners.Resize(MaxShadowsInView);
	TVector<RHI::RHITexturePtr> shadowMaps(MaxShadowsInView);
	for (uint32_t i = 0; i < MaxShadowsInView; i++)
	{
		m_shadowMapOwners[i] = InvalidShadowMapIndex;
		m_shadowMapSlots[i] = (i < m_csmShadowMaps.Num()) ? m_csmShadowMaps[i] : m_defaultShadowMap;
		shadowMaps[i] = m_shadowMapSlots[i];
	}

	m_shadowMaps = Sailor::RHI::Renderer::GetDriver()->AddSamplerToShaderBindings(m_lightsData, "shadowMaps", shadowMaps, 9);

	auto shaderBindingSet = m_lightsData;
	m_lightMatrices = Sailor::RHI::Renderer::GetDriver()->AddSsboToShaderBindings(shaderBindingSet, "lightsMatrices", sizeof(glm::mat4), LightingECS::MaxShadowsInView, 6);
	m_shadowIndices = Sailor::RHI::Renderer::GetDriver()->AddSsboToShaderBindings(shaderBindingSet, "shadowIndices", sizeof(uint32_t), LightingECS::LightsMaxNum, 7);
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
	m_csmSnapshots.Clear();
	m_shadowMapSlots.Clear();
	m_shadowMapOwners.Clear();
	m_localShadowAllocations.Clear();
	m_defaultShadowMap.Clear();
	m_shadowMaps.Clear();
	m_lightMatrices.Clear();
	m_shadowIndices.Clear();
	m_numLights = 0;
	m_shadowMapsMb = 0.0f;
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
			ObjectPtr ownerObject = light.m_owner;
			GameObjectPtr owner = ownerObject.StaticCast<GameObject>();
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
				const float sphereRadius = (std::max)(glm::abs(light.m_bounds.x),
					(std::max)(glm::abs(light.m_bounds.y), glm::abs(light.m_bounds.z)));
				const glm::vec3 worldPosition = ownerTransform.GetWorldPosition();

				if (frustum.ContainsSphere(Math::Sphere(worldPosition, sphereRadius)))
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

		for (uint32_t k = 0; k < lightCascadesMatrices.Num(); ++k)
		{
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

			if (snapshotIndex < m_csmSnapshots.Num())
			{
				m_csmSnapshots[snapshotIndex] = std::move(snapshot);
			}
			else
			{
				m_csmSnapshots.Emplace(std::move(snapshot));
			}

			updateShadowMaps.Emplace(std::move(cascade));
			++snapshotIndex;
		}
	}

	return updateShadowMaps;
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

	auto& driver = Sailor::RHI::Renderer::GetDriver();
	for (uint32_t slot : allocation.m_slots)
	{
		if (slot >= m_shadowMapSlots.Num())
		{
			continue;
		}

		m_shadowMapOwners[slot] = InvalidShadowMapIndex;
		m_shadowMapSlots[slot] = m_defaultShadowMap;
		driver->UpdateShaderBinding(m_lightsData, "shadowMaps", m_defaultShadowMap, slot);
	}

	m_shadowMapsMb = (std::max)(0.0f, m_shadowMapsMb - allocation.m_memoryMb);
	allocation = {};
}

bool LightingECS::EnsureLocalShadowAllocation(
	uint32_t componentIndex,
	ELightType lightType,
	ELightShadowQuality quality,
	uint64_t frame)
{
	if (m_localShadowAllocations.Num() <= componentIndex)
	{
		m_localShadowAllocations.Resize(static_cast<size_t>(componentIndex) + 1);
	}

	auto& current = m_localShadowAllocations[componentIndex];
	if (current.m_componentIndex == componentIndex &&
		current.m_lightType == lightType &&
		current.m_quality == quality &&
		!current.m_slots.IsEmpty())
	{
		current.m_lastUsedFrame = frame;
		return true;
	}

	ReleaseLocalShadowAllocation(componentIndex);

	const uint32_t mapCount = GetLocalShadowMapCount(lightType);
	if (mapCount == 0)
	{
		return false;
	}

	uint32_t firstSlot = InvalidShadowMapIndex;
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
			firstSlot = candidate;
			break;
		}
	}

	if (firstSlot == InvalidShadowMapIndex)
	{
		return false;
	}

	const uint32_t resolution = GetLocalShadowResolution(quality);
	const float allocationMb = static_cast<float>(resolution) *
		static_cast<float>(resolution) * 2.0f * static_cast<float>(mapCount) /
		(1024.0f * 1024.0f);
	if (m_shadowMapsMb + allocationMb > ShadowsMemoryBudgetMb)
	{
		return false;
	}

	const auto usage = RHI::ETextureUsageBit::ColorAttachment_Bit |
		RHI::ETextureUsageBit::TextureTransferSrc_Bit |
		RHI::ETextureUsageBit::TextureTransferDst_Bit |
		RHI::ETextureUsageBit::Sampled_Bit;
	auto& driver = Sailor::RHI::Renderer::GetDriver();

	LocalLightShadowAllocation allocation{};
	allocation.m_componentIndex = componentIndex;
	allocation.m_lightType = lightType;
	allocation.m_quality = quality;
	allocation.m_lastUsedFrame = frame;
	allocation.m_memoryMb = allocationMb;
	allocation.m_slots.Reserve(mapCount);
	allocation.m_snapshots.Resize(mapCount);

	for (uint32_t face = 0; face < mapCount; ++face)
	{
		const uint32_t slot = firstSlot + face;
		auto shadowMap = driver->CreateRenderTarget(
			glm::ivec2(resolution),
			1,
			ShadowMapFormat,
			RHI::ETextureFiltration::Nearest,
			RHI::ETextureClamping::Clamp,
			usage);

		char debugName[96];
		sprintf_s(debugName, sizeof(debugName),
			"Shadow Map, Local Light: %u, Face: %u", componentIndex, face);
		driver->SetDebugName(shadowMap, debugName);

		m_shadowMapOwners[slot] = componentIndex;
		m_shadowMapSlots[slot] = shadowMap;
		driver->UpdateShaderBinding(m_lightsData, "shadowMaps", shadowMap, slot);
		allocation.m_slots.Add(slot);
	}

	m_shadowMapsMb += allocationMb;
	m_localShadowAllocations[componentIndex] = std::move(allocation);
	return true;
}

void LightingECS::ReleaseUnusedLocalShadowAllocations(uint64_t frame)
{
	for (uint32_t componentIndex = 0;
		componentIndex < m_localShadowAllocations.Num();
		++componentIndex)
	{
		const auto& allocation = m_localShadowAllocations[componentIndex];
		if (allocation.m_componentIndex == componentIndex &&
			allocation.m_lastUsedFrame != frame)
		{
			ReleaseLocalShadowAllocation(componentIndex);
		}
	}
}

TVector<RHI::RHIUpdateShadowMapCommand> LightingECS::PrepareLocalShadowPasses(
	const RHI::RHISceneViewPtr& sceneView,
	const TVector<RHI::RHILightProxy>& spotLights,
	const TVector<RHI::RHILightProxy>& pointLights,
	TVector<uint32_t>& shadowIndices)
{
	TVector<RHI::RHIUpdateShadowMapCommand> updateShadowMaps{};
	const uint64_t frame = GetWorld()->GetCurrentFrame();

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
			if (light.m_shadowType == RHI::EShadowType::None ||
				(light.m_type != ELightType::Point && light.m_type != ELightType::Spot) ||
				!EnsureLocalShadowAllocation(
					lightProxy.m_index,
					light.m_type,
					light.m_shadowQuality,
					frame))
			{
				continue;
			}

			auto& allocation = m_localShadowAllocations[lightProxy.m_index];
			const uint32_t firstSlot = allocation.m_slots[0];
			shadowIndices[lightProxy.m_index] = firstSlot |
				(light.m_shadowFilter == ELightShadowFilter::Soft ? SoftShadowMapBit : 0u);

			ObjectPtr ownerObject = light.m_owner;
			GameObjectPtr owner = ownerObject.StaticCast<GameObject>();
			if (!owner)
			{
				continue;
			}

			const auto& transform = owner->GetTransformComponent();
			const glm::vec3 position = transform.GetWorldPosition();
			const float farPlane = (std::max)(
				glm::abs(light.m_bounds.x),
				(std::max)(glm::abs(light.m_bounds.y), glm::abs(light.m_bounds.z)));
			if (farPlane <= 0.05f)
			{
				shadowIndices[lightProxy.m_index] = InvalidShadowMapIndex;
				continue;
			}

			const float nearPlane = (std::max)(0.01f, farPlane * 0.001f);
			TVector<glm::mat4> lightMatrices{};
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
				lightMatrices.Add(Math::PerspectiveRH(
					glm::radians(fieldOfView), 1.0f, nearPlane, farPlane) * view);
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
					lightMatrices.Add(projection * glm::lookAtRH(
						position,
						position + directions[face],
						upVectors[face]));
				}
			}

			for (uint32_t face = 0; face < lightMatrices.Num(); ++face)
			{
				auto& cachedState = allocation.m_snapshots[face];
				if (!sceneView->m_bHasCustomDepthShadowCasters &&
					cachedState.CanReuse(
					lightProxy.m_index,
					RHI::EShadowType::PCF,
					lightMatrices[face],
					sceneView->m_shadowCastersRevision))
				{
					continue;
				}

				Math::Frustum shadowFrustum(lightMatrices[face]);
				RHI::RHIUpdateShadowMapCommand shadowPass{};
				shadowPass.m_shadowMap = m_shadowMapSlots[allocation.m_slots[face]];
				shadowPass.m_lightMatrix = lightMatrices[face];
				shadowPass.m_lighMatrixIndex = allocation.m_slots[face];
				shadowPass.m_shadowType = RHI::EShadowType::PCF;
				shadowPass.m_meshList = sceneView->TraceShadowCasters(shadowFrustum);

				CSMLightState snapshot{};
				snapshot.m_componentIndex = lightProxy.m_index;
				snapshot.m_shadowType = RHI::EShadowType::PCF;
				snapshot.m_lightMatrix = lightMatrices[face];
				snapshot.m_sceneRevision = sceneView->m_shadowCastersRevision;
				snapshot.m_snapshot.Reserve(shadowPass.m_meshList.Num());
				for (const auto& caster : shadowPass.m_meshList)
				{
					if (caster)
					{
						snapshot.m_snapshot.Add({ caster->m_staticMeshEcs, caster->m_frame });
					}
				}
				snapshot.m_snapshot.Sort([](const auto& lhs, const auto& rhs)
					{
						return lhs.m_first < rhs.m_first;
					});
				cachedState = std::move(snapshot);
				updateShadowMaps.Emplace(std::move(shadowPass));
			}
		}
	};

	prepareLights(spotLights);
	prepareLights(pointLights);
	return updateShadowMaps;
}

void LightingECS::FillLightingData(RHI::RHISceneViewPtr& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	uint32_t snapshotIndex = 0;
	TVector<uint32_t> shadowIndices(GetGpuLightSlotsCount(m_components.Num()));
	for (auto& shadowIndex : shadowIndices)
	{
		shadowIndex = InvalidShadowMapIndex;
	}

	for (uint32_t i = 0; i < sceneView->m_cameraTransforms.Num(); i++)
	{
		const auto& camera = sceneView->m_cameras[i];

		Math::Frustum frustum;
		frustum.ExtractFrustumPlanes(sceneView->m_cameraTransforms[i].Matrix(), camera.GetAspect(), camera.GetFov(), camera.GetZNear(), camera.GetZFar());

		// Sort all the lights per camera
		TVector<RHI::RHILightProxy> directionalLights;
		TVector<RHI::RHILightProxy> sortedSpotLights;
		TVector<RHI::RHILightProxy> sortedPointLights;

		GetLightsInFrustum(frustum, sceneView->m_cameraTransforms[i], directionalLights, sortedPointLights, sortedSpotLights);

		auto updateShadowMaps = PrepareCSMPasses(
			sceneView,
			sceneView->m_cameraTransforms[i],
			camera,
			directionalLights,
			snapshotIndex);
		auto localShadowMaps = PrepareLocalShadowPasses(
			sceneView,
			sortedSpotLights,
			sortedPointLights,
			shadowIndices);
		for (auto& localShadowMap : localShadowMaps)
		{
			updateShadowMaps.Emplace(std::move(localShadowMap));
		}
		sceneView->m_shadowMapsToUpdate.Add(std::move(updateShadowMaps));
	}

	if (m_csmSnapshots.Num() > snapshotIndex)
	{
		m_csmSnapshots.Resize(snapshotIndex);
	}

	if (!shadowIndices.IsEmpty())
	{
		auto commands = Sailor::RHI::Renderer::GetDriverCommands();
		commands->UpdateShaderBinding(
			GetWorld()->GetCommandList(),
			m_shadowIndices,
			shadowIndices.GetData(),
			shadowIndices.Num() * sizeof(uint32_t),
			0);
	}
	ReleaseUnusedLocalShadowAllocations(GetWorld()->GetCurrentFrame());

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
