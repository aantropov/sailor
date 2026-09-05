#include "ECS/LightingECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "GlobalIllumination/GISettings.h"
#include "RHI/DebugContext.h"
#include "RHI/SceneView.h"
#include "Settings/GraphicsSettings.h"

#include <algorithm>
#include <utility>

using namespace Sailor;
using namespace Sailor::Tasks;

void LightingECS::FillLightingData(RHI::RHISceneViewPtr& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	if (!sceneView || !sceneView->m_submissionContext)
	{
		SAILOR_LOG_ERROR("LightingECS::FillLightingData requires an acquired render submission flight.");
		return;
	}
	const uint32_t flightSlot = sceneView->m_submissionContext->GetFlightSlot();
	if (m_shadowFlightResources.Num() <= flightSlot)
	{
		m_shadowFlightResources.Resize(static_cast<size_t>(flightSlot) + 1u);
	}
	auto& flightResources = m_shadowFlightResources[flightSlot];
	m_writableLocalShadowAtlases.reset();
	uint32_t snapshotIndex = 0;
	const glm::ivec2 viewportExtent = App::GetMainWindow()->GetRenderArea();
	const Settings::GraphicsExtent renderExtent =
		Settings::ResolveRenderDimensions(static_cast<uint32_t>((std::max)(viewportExtent.x, 1)),
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
		frustum.ExtractFrustumPlanes(sceneView->m_cameraTransforms[i].Matrix(),
			camera.GetAspect(),
			camera.GetFov(),
			camera.GetZNear(),
			camera.GetZFar());

		// Sort all the lights per camera
		m_directionalLightsScratch.Clear(false);
		m_pointLightsScratch.Clear(false);
		m_spotLightsScratch.Clear(false);

		GetLightsInFrustum(frustum,
			sceneView->m_cameraTransforms[i],
			m_directionalLightsScratch,
			m_pointLightsScratch,
			m_spotLightsScratch);

		const uint32_t cameraCsmSnapshotStart = snapshotIndex;
		PrepareCSMPasses(sceneView,
			sceneView->m_cameraTransforms[i],
			camera,
			m_directionalLightsScratch,
			flightSlot,
			flightResources,
			snapshotIndex,
			updateShadowMaps);
		PrepareLocalShadowPasses(sceneView,
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
			cascadeIndex < NumCascades && cameraCsmSnapshotStart + cascadeIndex < flightResources.m_csmSnapshots.Num();
			++cascadeIndex)
		{
			shadowMatrices[cascadeIndex] =
				flightResources.m_csmSnapshots[cameraCsmSnapshotStart + cascadeIndex].m_lightMatrix;
		}
		for (const auto& allocation : m_localShadowAllocations)
		{
			if (allocation.m_componentIndex == InvalidShadowMapIndex ||
				allocation.m_componentIndex >= shadowIndices.Num() ||
				shadowIndices[allocation.m_componentIndex] == InvalidShadowMapIndex)
			{
				continue;
			}
			if (flightResources.m_localShadowSnapshots.Num() <= allocation.m_componentIndex)
			{
				continue;
			}
			const auto& flightSnapshots = flightResources.m_localShadowSnapshots[allocation.m_componentIndex];
			const uint32_t numFaces =
				static_cast<uint32_t>((std::min)(allocation.m_slots.Num(), flightSnapshots.Num()));
			for (uint32_t face = 0u; face < numFaces; ++face)
			{
				const uint32_t slot = allocation.m_slots[face];
				if (shadowMatrices.Num() <= slot)
				{
					const size_t previousSize = shadowMatrices.Num();
					shadowMatrices.Resize(static_cast<size_t>(slot) + 1u);
					for (size_t matrixIndex = previousSize; matrixIndex < shadowMatrices.Num(); ++matrixIndex)
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

void LightingECS::GetGlobalIlluminationBakeLightProxies(TVector<Raytracing::LightProxy>& outLights) const
{
	CollectLightProxies(outLights, true);
}

void LightingECS::CollectLightProxies(TVector<Raytracing::LightProxy>& outLights,
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
		if (!glm::any(glm::greaterThan(light.m_intensity, glm::vec3(0.0f))))
		{
			continue;
		}
		const bool bContributes = bGlobalIlluminationBakeContributorsOnly
									  ? ContributesToBakedGlobalIllumination(light.m_globalIlluminationMode)
									  : ContributesToRealtimeLighting(light.m_globalIlluminationMode);
		if (!bContributes)
		{
			continue;
		}

		GameObject* pOwner = light.m_owner ? static_cast<GameObject*>(light.m_owner.GetRawPtr()) : nullptr;
		if (!pOwner)
		{
			continue;
		}
		if (bGlobalIlluminationBakeContributorsOnly && !IsGlobalIlluminationBakeContributor(pOwner->GetMobilityType()))
		{
			continue;
		}

		const auto& transform = pOwner->GetTransformComponent();

		Raytracing::LightProxy lightProxy{};
		lightProxy.m_type = light.m_type;
		lightProxy.m_worldPosition = transform.GetWorldPosition();
		lightProxy.m_direction = glm::normalize(transform.GetForwardVector());
		lightProxy.m_intensity = light.m_intensity;
		lightProxy.m_indirectLightingIntensity = light.m_indirectLightingIntensity;
		lightProxy.m_bounds = glm::vec3(light.m_radius);
		lightProxy.m_cutOff = vec2(glm::cos(glm::radians(light.m_cutOff.x)), glm::cos(glm::radians(light.m_cutOff.y)));
		lightProxy.m_bCastShadows = light.m_shadowType != RHI::EShadowType::None;

		outLights.Add(lightProxy);
	}
}
