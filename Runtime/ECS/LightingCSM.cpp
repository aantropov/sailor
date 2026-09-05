#include "ECS/LightingECS.h"
#include "ECS/LightingECSInternal.h"
#include "FrameGraph/ShadowPrepassNode.h"
#include "RHI/DebugContext.h"
#include "RHI/RenderTarget.h"
#include "RHI/SceneView.h"
#include "Settings/GraphicsSettings.h"

#include <algorithm>
#include <array>
#include <utility>

using namespace Sailor;
using namespace Sailor::LightingECSInternal;
using namespace Sailor::Tasks;

bool CSMLightState::CanReuse(uint32_t componentIndex,
	RHI::EShadowType shadowType,
	const glm::mat4& lightMatrix,
	uint64_t sceneRevision,
	const TSharedPtr<TVector<RHI::RHISceneVersionPtr>>& sceneVersions,
	const Math::Frustum& shadowFrustum,
	const RHI::RHISubmissionCompletionTokenPtr& currentSubmissionToken,
	uint64_t animationRevision) const
{
	if (m_componentIndex != componentIndex || m_shadowType != shadowType ||
		!Math::AreExactlyEqual(m_lightMatrix, lightMatrix) || !m_submissionToken ||
		(!m_submissionToken->IsSuccessful() &&
			(!m_submissionToken->IsPending() || m_submissionToken != currentSubmissionToken)) ||
		!m_payloadCompletionToken ||
		(!m_payloadCompletionToken->IsSuccessful() &&
			(!m_payloadCompletionToken->IsPending() || m_submissionToken != currentSubmissionToken)))
	{
		return false;
	}
	if (m_bContainsDynamicCasters && m_submissionToken != currentSubmissionToken)
	{
		return false;
	}
	if (m_bContainsAnimatedCasters && m_animationRevision != animationRevision)
	{
		return false;
	}
	if (m_sceneRevision == sceneRevision)
	{
		return true;
	}
	if (!m_casterSceneVersions || !sceneVersions || m_casterSceneVersions->Num() != sceneVersions->Num())
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
		if (!previous || !current || current->HasShadowChangesIntersecting(*previous, shadowFrustum))
		{
			return false;
		}
	}

	return true;
}

void LightingECS::PrepareCSMPasses(const RHI::RHISceneViewPtr& sceneView,
	const Math::Transform& cameraTransform,
	const CameraData& cameraData,
	const TVector<RHI::RHILightProxy>& directionalLights,
	uint32_t flightSlot,
	LightingShadowFlightResources& flightResources,
	uint32_t& snapshotIndex,
	TVector<RHI::RHIUpdateShadowMapCommand>& outUpdateShadowMaps)
{
	SAILOR_PROFILE_FUNCTION();

	outUpdateShadowMaps.Reserve(outUpdateShadowMaps.Num() + directionalLights.Num() * NumCascades);
	const auto casterSceneVersions = sceneView->GetRetainedSceneVersions();
	const auto submissionToken = sceneView->GetOrCreateSubmissionCompletionToken();
	auto& csmSnapshots = flightResources.m_csmSnapshots;
	const auto& graphicsProfile = App::GetActiveGraphicsSettings();

	for (const auto& directionalLight : directionalLights)
	{
		ShadowPrepassNode::CalculateLightProjectionForCascades(directionalLight.m_lightMatrix,
			cameraTransform.Matrix(),
			cameraData.GetAspect(),
			cameraData.GetFov(),
			cameraData.GetZNear(),
			cameraData.GetZFar(),
			m_cascadeProjectionScratch);
		const auto& lightCascadesMatrices = m_cascadeProjectionScratch;
		const uint32_t numCascades =
			static_cast<uint32_t>((std::min)(lightCascadesMatrices.Num(), static_cast<size_t>(NumCascades)));

		std::array<Math::Frustum, NumCascades> frustums{};
		std::array<glm::mat4, NumCascades> lightMatrices{};
		std::array<uint8_t, NumCascades> cascadeNeedsUpdate{};
		const bool bForceCustomDepthShadowUpdate = sceneView->m_bHasCustomDepthShadowCasters;
		bool bCanReuseAllCascades = true;
		for (uint32_t cascadeIndex = 0; cascadeIndex < numCascades; ++cascadeIndex)
		{
			lightMatrices[cascadeIndex] = lightCascadesMatrices[cascadeIndex] * directionalLight.m_lightMatrix;
			frustums[cascadeIndex].ExtractFrustumPlanes(lightMatrices[cascadeIndex]);
			const RHI::EShadowType shadowType =
				cascadeIndex > 0 ? RHI::EShadowType::PCF : directionalLight.m_shadowType;
			const uint32_t currentSnapshotIndex = snapshotIndex + cascadeIndex;
			const bool bCanReuseCascade = !bForceCustomDepthShadowUpdate && currentSnapshotIndex < csmSnapshots.Num() &&
										  currentSnapshotIndex < flightResources.m_csmShadowMaps.Num() &&
										  flightResources.m_csmShadowMaps[currentSnapshotIndex] &&
										  csmSnapshots[currentSnapshotIndex].m_shadowMap ==
											  flightResources.m_csmShadowMaps[currentSnapshotIndex] &&
										  csmSnapshots[currentSnapshotIndex].CanReuse(directionalLight.m_index,
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
			for (uint32_t cascadeIndex = 0u; cascadeIndex < numCascades; ++cascadeIndex)
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

		const glm::mat4 broadLightMatrix =
			ShadowPrepassNode::CalculateLightProjectionMatrix(directionalLight.m_lightMatrix,
				cameraTransform.Matrix(),
				cameraData.GetAspect(),
				cameraData.GetFov(),
				cameraData.GetZNear(),
				(std::min)(cameraData.GetZFar(), ShadowMaxDistance),
				10.0f,
				glm::ivec2(0),
				ShadowCasterDepthExtension) *
			directionalLight.m_lightMatrix;
		Math::Frustum broadFrustum;
		broadFrustum.ExtractFrustumPlanes(broadLightMatrix);
		sceneView->TraceShadowCasters(broadFrustum, glm::vec3(cameraTransform.m_position), m_csmBroadCastersScratch);
		const auto& shadowCasters = m_csmBroadCastersScratch;
		std::array<Tasks::TaskPtr<TVector<RHI::RHIVisibleShadowCaster>>, NumCascades> cascadeCasterTasks{};
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
						if (caster.GetSource() && frustums[cascadeIndex].OverlapsAABB(caster.GetWorldBounds()))
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
				cascade.m_meshList, snapshot.m_bContainsDynamicCasters, snapshot.m_bContainsAnimatedCasters);

			const bool bEvsmCascade = cascade.m_shadowType == RHI::EShadowType::EVSM;
			const glm::ivec2 shadowMapExtent(graphicsProfile.GetShadowCascadeResolution(k));
			const RHI::EFormat shadowMapFormat = GetCsmShadowMapFormat(cascade.m_shadowType);
			if (flightResources.m_csmShadowMaps.Num() <= snapshotIndex)
			{
				flightResources.m_csmShadowMaps.Resize(static_cast<size_t>(snapshotIndex) + 1u);
			}
			auto& writableShadowMap = flightResources.m_csmShadowMaps[snapshotIndex];
			if (writableShadowMap && (writableShadowMap->GetFormat() != shadowMapFormat ||
										 writableShadowMap->GetExtent().x != shadowMapExtent.x ||
										 writableShadowMap->GetExtent().y != shadowMapExtent.y))
			{
				const auto staleShadowMap = writableShadowMap;
				const float staleMemoryMb = CalculateCsmShadowMapMemoryMb(staleShadowMap);
				m_csmShadowMapsMb = (std::max)(0.0f, m_csmShadowMapsMb - staleMemoryMb);
				m_shadowMapsMb = (std::max)(0.0f, m_shadowMapsMb - staleMemoryMb);
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
								   RHI::ETextureUsageBit::TextureTransferDst_Bit | RHI::ETextureUsageBit::Sampled_Bit;
				writableShadowMap = RHI::Renderer::GetDriver()->CreateRenderTarget(shadowMapExtent,
					1,
					shadowMapFormat,
					bEvsmCascade ? RHI::ETextureFiltration::Linear : RHI::ETextureFiltration::Nearest,
					RHI::ETextureClamping::Clamp,
					usage);
				if (writableShadowMap)
				{
					const float shadowMapMemoryMb = CalculateCsmShadowMapMemoryMb(writableShadowMap);
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
			sprintf_s(csmDebugName,
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
				snapshotIndex < csmSnapshots.Num() ? csmSnapshots[snapshotIndex].m_payloadCompletionToken
												   : RHI::RHISubmissionCompletionTokenPtr{});
			cascade.m_payloadCompletionToken = snapshot.m_payloadCompletionToken;

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
