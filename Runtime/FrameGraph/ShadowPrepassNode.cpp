#include "ShadowPrepassNode.h"
#include "Core/StringHash.h"
#include "RHI/PackedDrawCommands.hpp"
#include "RHI/MaterialPreparationCache.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "Settings/GraphicsSettings.h"
#include "RHI/RenderTarget.h"
#include "RHI/Types.h"
#include "RHI/VertexDescription.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "ECS/LightingECS.h"

#include <algorithm>
#include <limits>

using namespace Sailor;
using namespace Sailor::RHI;

namespace
{
	ShadowPrepassNode::PerInstanceData MakeInstanceData(
		const glm::mat4& model, const RHIMesh& mesh, uint32_t materialInstance,
		uint32_t skeletonOffset, float baseColorAlpha, uint32_t baseColorSampler, float alphaCutoff)
	{
		ShadowPrepassNode::PerInstanceData data;
		data.model = model;
		data.sphereBounds = mesh.m_bounds.ToSphere().GetVec4();
		data.materialInstance = materialInstance;
		data.skeletonOffset = skeletonOffset;
		data.bakedVolumeScale = vec4(mesh.m_bakedVolumeScale, 1.0f);
		data.baseColorAlpha = baseColorAlpha;
		data.baseColorSampler = baseColorSampler;
		data.alphaCutoff = alphaCutoff;
		return data;
	}
}

RHI::RHIMaterialPtr ShadowPrepassNode::SelectShadowMaterial(
	RHI::RHIVertexDescriptionPtr vertex, RHI::EShadowType shadowType, bool bSkinned, bool bMasked,
	const ShaderSetPtr& sourceShader, const RHI::RHIMaterialPtr& sourceMaterial,
	const RHI::RHIMaterialVersionPtr& sourceVersion, uint64_t frame)
{
	auto material = GetOrAddShadowMaterial(vertex, shadowType, bSkinned, bMasked);
	if (sourceMaterial)
	{
		if (auto custom = GetOrAddCustomShadowMaterial(
			sourceShader, sourceMaterial, sourceVersion, vertex, shadowType, bMasked, frame))
		{
			material = std::move(custom);
		}
	}
	return material;
}

RHI::RHIMaterialPtr ShadowPrepassNode::GetOrAddShadowMaterial(RHI::RHIVertexDescriptionPtr vertexDescription, RHI::EShadowType shadowType, bool bSkinned, bool bMasked)
{
	auto& materials = shadowType == EShadowType::EVSM ?
		(bMasked ?
			(bSkinned ? m_skinnedMaskedShadowMaterials_Evsm : m_maskedShadowMaterials_Evsm) :
			(bSkinned ? m_skinnedShadowMaterials_Evsm : m_shadowMaterials_Evsm)) :
		(bMasked ?
			(bSkinned ? m_skinnedMaskedShadowMaterials_Pcf : m_maskedShadowMaterials_Pcf) :
			(bSkinned ? m_skinnedShadowMaterials_Pcf : m_shadowMaterials_Pcf));
	auto& material = materials[vertexDescription->GetVertexAttributeBits()];

	if (!material)
	{
		auto shaderFileId = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ShadowCaster.shader");
		ShaderSetPtr pShader;

		TVector<std::string> defines;
		if (shadowType == EShadowType::EVSM)
		{
			defines.Add("EVSM");
		}
		if (bSkinned)
		{
			defines.Add("SKINNING");
		}
		if (bMasked)
		{
			defines.Add("MASKED");
		}

		if (App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderFileId->GetFileId(), pShader, defines))
		{
			check(pShader->IsReady());

			const ECullMode cullMode = bMasked ? ECullMode::None : ECullMode::Back;
			const float shadowBias = GetRasterShadowBias(
				shadowType,
				App::GetActiveGraphicsSettings().m_shadowBias);
			RenderState renderState = RHI::RenderState(
				true,
				true,
				shadowBias,
				false,
				cullMode,
				EBlendMode::None,
				EFillMode::Fill,
				"Shadow"_h.GetHash(),
				false,
				EDepthCompare::GreaterOrEqual);
			material = RHI::Renderer::GetDriver()->CreateMaterial(vertexDescription, RHI::EPrimitiveTopology::TriangleList, renderState, pShader);
		}
	}

	return material;
}

RHI::RHIMaterialPtr ShadowPrepassNode::GetOrAddCustomShadowMaterial(
	const ShaderSetPtr& sourceShader,
	const RHI::RHIMaterialPtr& sourceMaterial,
	const RHI::RHIMaterialVersionPtr& sourceMaterialVersion,
	RHI::RHIVertexDescriptionPtr vertexDescription,
	RHI::EShadowType shadowType,
	bool bMasked,
	uint64_t frame)
{
	if (!sourceShader || !sourceMaterial ||
		!sourceMaterialVersion || !sourceMaterialVersion->GetBindings())
	{
		return nullptr;
	}

	auto shaderCompiler = App::GetSubmodule<ShaderCompiler>();
	auto shaderAsset = shaderCompiler->LoadShaderAsset(
		sourceShader->GetFileId());
	if (!shaderAsset || !shaderAsset->GetSupportedDefines().Contains("PACKED_SHADOW_CASTER"))
	{
		return nullptr;
	}

	const CustomShadowMaterialKey cacheKey{
		sourceMaterial.GetRawPtr(),
		vertexDescription->GetVertexAttributeBits(),
		shadowType,
		bMasked };
	auto& entry = m_customShadowMaterials[cacheKey];
	entry.m_lastUsedFrame = frame;
	if (entry.m_sourceVersion == sourceMaterialVersion && entry.m_material)
	{
		return entry.m_material;
	}

	TVector<std::string> defines = sourceShader->GetDefines();
	if (!defines.Contains("PACKED_SHADOW_CASTER"))
	{
		defines.Add("PACKED_SHADOW_CASTER");
	}
	if (bMasked && shaderAsset->GetSupportedDefines().Contains("ALPHA_CUTOUT") &&
		!defines.Contains("ALPHA_CUTOUT"))
	{
		defines.Add("ALPHA_CUTOUT");
	}
	if (shadowType == EShadowType::EVSM && !defines.Contains("EVSM"))
	{
		defines.Add("EVSM");
	}

	ShaderSetPtr shadowShader;
	if (!shaderCompiler->LoadShader_Immediate(
		sourceShader->GetFileId(),
		shadowShader,
		defines) || !shadowShader->IsReady())
	{
		return nullptr;
	}

	const auto& sourceState = sourceMaterial->GetRenderState();
	const float shadowBias = GetRasterShadowBias(
		shadowType,
		App::GetActiveGraphicsSettings().m_shadowBias);
	RenderState shadowState(true,
		true,
		sourceState.GetDepthBias() + shadowBias,
		true,
		sourceState.GetCullMode(),
		EBlendMode::None,
		sourceState.GetFillMode(),
		"Shadow"_h.GetHash(),
		false,
		EDepthCompare::GreaterOrEqual);
	auto material = RHI::Renderer::GetDriver()->CreateMaterial(
		vertexDescription,
		RHI::EPrimitiveTopology::TriangleList,
		shadowState,
		shadowShader,
		sourceMaterialVersion->GetBindings());
	if (material)
	{
		entry.m_sourceVersion = sourceMaterialVersion;
		entry.m_material = material;
	}
	return material;
}

void ShadowPrepassNode::EvictCustomShadowMaterials(uint64_t frame)
{
	constexpr uint64_t RetentionFrames = 5u;
	TVector<CustomShadowMaterialKey> expired;
	for (const auto& entry : m_customShadowMaterials)
	{
		if (frame > entry.Second()->m_lastUsedFrame && frame - entry.Second()->m_lastUsedFrame > RetentionFrames)
		{
			expired.Add(entry.First());
		}
	}
	// Recorded packets and command lists retain their own exact material generations.
	for (const auto& key : expired)
	{
		m_customShadowMaterials.Remove(key);
	}
}

Tasks::TaskPtr<void, void> ShadowPrepassNode::Prepare(RHIFrameGraphPtr frameGraph, RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	if (!sceneView.m_submissionContext)
	{
		return {};
	}
	std::string_view virtualizeInstancePayloadsSetting;
	const bool bVirtualizeInstancePayloads =
		!TryGetString("VirtualizeInstancePayloads"_h, virtualizeInstancePayloadsSetting) ||
		virtualizeInstancePayloadsSetting != "false";

	// Shader cache misses can wait for Worker jobs, so material/arena preparation
	// uses RHI. Independent packet finalization runs on the general Worker pool.
	return Tasks::CreateTask("Prepare ShadowPrepassNode"_h,
		[this, holdRhiResources = frameGraph, &sceneView, bVirtualizeInstancePayloads]()
		{
			SAILOR_PROFILE_SCOPE("Prepare shadow packets");
			// These are immutable pass materials; custom derivatives already retain
			// the source generation resolved for this submission.
			RHIMaterialPreparationCache preparedMaterials(0ull);
			auto submissionResources = sceneView.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(
				this, sceneView.m_cameraIndex, 0u);
			const uint32_t NumShadowPasses = static_cast<uint32_t>(sceneView.m_shadowMapsToUpdate.Num());
			submissionResources->m_activeShadowViews.Clear(false);
			submissionResources->m_activeShadowViews.Reserve(NumShadowPasses);
			auto& bShadowPayloadComplete = submissionResources->m_shadowPayloadComplete;
			bShadowPayloadComplete.Clear(false);
			bShadowPayloadComplete.Resize(NumShadowPasses);
			for (uint32_t passIndex = 0; passIndex < NumShadowPasses; ++passIndex)
			{
				const auto& shadowPass = sceneView.m_shadowMapsToUpdate[passIndex];
				size_t viewKey = Fnv1aOffsetBasis;
				HashCombine(
					viewKey,
					shadowPass.m_lighMatrixIndex,
					static_cast<uint32_t>(shadowPass.m_shadowType));
				auto& viewResources = submissionResources->m_shadowViewCache[viewKey];
				if (!viewResources)
				{
					viewResources = TSharedPtr<SubmissionResources::ShadowViewResources>::Make();
				}
				viewResources->Begin(viewKey);
				submissionResources->m_activeShadowViews.Add(viewResources);

				bShadowPayloadComplete[passIndex].fill(true);
			}

			TVector<Tasks::TaskPtr<void, void>> finalizeTasks;
			finalizeTasks.Reserve(NumShadowPasses);
			m_syncSharedResources.Lock();
			Framegraph::Details::EvictTextureBindingCache(m_textureBindingCache, sceneView.m_frame);
			for (uint32_t passIndex = 0; passIndex < NumShadowPasses; ++passIndex)
			{
				if (bVirtualizeInstancePayloads)
				{
					BuildStableArenas(sceneView, *submissionResources, preparedMaterials, passIndex);
				}
				BuildVisiblePacket(sceneView, *submissionResources, preparedMaterials, passIndex, bVirtualizeInstancePayloads);
				auto finalize = Tasks::CreateTask("Finalize shadow packet"_h,
					[view = submissionResources->m_activeShadowViews[passIndex]]()
					{
						SAILOR_PROFILE_SCOPE("Finalize shadow packet");
						view->m_packet.Finalize(false);
					}, EThreadType::Worker);
				finalizeTasks.Add(finalize);
				finalize->Run();
			}
			m_pagedArenaCache.Evict(sceneView.m_frame);
			EvictCustomShadowMaterials(sceneView.m_frame);
			m_syncSharedResources.Unlock();

			// Workers own separate view packets. Their draws and arena pages already
			// retain the captured material versions; only completion waits for finalization.
			for (const auto& task : finalizeTasks)
			{
				task->Wait();
			}
			for (uint32_t passIndex = 0u; passIndex < NumShadowPasses; ++passIndex)
			{
				const auto& shadowPass = sceneView.m_shadowMapsToUpdate[passIndex];
				bool bPassPayloadComplete = true;
				for (bool bPayloadComplete : bShadowPayloadComplete[passIndex])
				{
					bPassPayloadComplete &= bPayloadComplete;
				}
				if (shadowPass.m_payloadCompletionToken)
				{
					shadowPass.m_payloadCompletionToken->Complete(
						bPassPayloadComplete);
				}
			}
		}, EThreadType::RHI);
}

void ShadowPrepassNode::BuildStableArenas(const RHISceneViewSnapshot& sceneView,
	SubmissionResources& resources, RHIMaterialPreparationCache& preparedMaterials, uint32_t passIndex)
{
	SAILOR_PROFILE_SCOPE("Build shadow stable arenas");
	const auto& shadowPass = sceneView.m_shadowMapsToUpdate[passIndex];
	auto& viewResources = *resources.m_activeShadowViews[passIndex];
	auto& payloadComplete = resources.m_shadowPayloadComplete[passIndex];
	const uint64_t materialSubmissionId = sceneView.m_submissionContext->GetSubmissionId();
	const size_t opaqueQueueTag = "Opaque"_h.GetHash();
	const size_t maskedQueueTag = "Masked"_h.GetHash();
	RHIPackedDrawSceneState sceneState{ sceneView.m_sceneVersions };
	HashCombine(sceneState.m_configurationRevision, static_cast<uint32_t>(shadowPass.m_shadowType),
		sceneView.m_submissionContext->GetMaterialRevision());
	for (const EMobilityType mobility : { EMobilityType::Static, EMobilityType::Stationary })
	{
		const size_t arenaPayloadIndex = TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(mobility);
		size_t payloadRevision = Fnv1aOffsetBasis;
		HashCombine(payloadRevision, arenaPayloadIndex, static_cast<uint32_t>(shadowPass.m_shadowType),
			sceneView.m_submissionContext->GetMaterialRevision(), sceneView.GetMobilityRevision(mobility));
		size_t arenaCacheSlot = Fnv1aOffsetBasis;
		HashCombine(arenaCacheSlot, static_cast<uint32_t>(shadowPass.m_shadowType), arenaPayloadIndex);
		if (auto payload = m_pagedArenaCache.Find(arenaCacheSlot, payloadRevision, sceneView.m_frame))
		{
			viewResources.m_packet.UseSharedArenaPayload(mobility, std::move(payload));
			continue;
		}
		m_arenaChanges.Gather(sceneState, m_pagedArenaCache.GetSceneState(arenaCacheSlot), mobility, true);
		m_pagedArenaCache.BeginUpdate(arenaCacheSlot, payloadRevision, sceneView.m_frame, sceneState);
		for (const auto& proxy : m_arenaChanges.m_removed)
			m_pagedArenaCache.RemoveRange(BuildPackedDrawRangeKey(proxy.m_handle, proxy.m_record->m_producerKey, proxy.m_resource));
		auto& rangeInstances = resources.m_arenaRangeInstances;
		auto& rangeStableKeys = resources.m_arenaRangeStableKeys;
		auto& rangeMaterialVersionRuns = resources.m_arenaRangeMaterialVersionRuns;
		auto buildRange = [&](const RHIVisibleShadowCaster& proxy)
		{
			const auto* source = proxy.GetSource();
			if (!source)
			{
				return;
			}
			const uint64_t rangeKey =
				BuildPackedDrawRangeKey(proxy.m_handle, proxy.GetProducerKey(), proxy.m_resource);
			size_t rangeRevision = proxy.m_resource->m_shadowRevision;
			HashCombine(rangeRevision, static_cast<uint32_t>(shadowPass.m_shadowType),
				proxy.GetContentRevision(), std::hash<glm::mat4>{}(proxy.GetWorldMatrix()),
				proxy.GetSkeletonOffset());
			HashCombine(rangeRevision, proxy.m_record->m_materialRevision,
				proxy.m_record->m_shadowRevision, proxy.m_record->m_renderFlags);
			for (const auto& shadowMesh : source->m_meshes)
			{
				if (shadowMesh.m_customDepthMaterial)
				{
					const auto materialVersion =
						shadowMesh.m_customDepthMaterial->GetVersionForSubmission(
							materialSubmissionId);
					HashCombine(
						rangeRevision, materialVersion ? materialVersion->GetVersionId() : 0ull);
				}
			}
			for (const auto& group : proxy.m_resource->m_proxy.m_instancedGroups)
			{
				for (const auto& material : group.m_materials)
				{
					if (material && material->GetRenderState().IsRequiredCustomDepthShader())
					{
						const auto materialVersion =
							material->GetVersionForSubmission(materialSubmissionId);
						HashCombine(rangeRevision,
							materialVersion ? materialVersion->GetVersionId() : 0ull);
					}
				}
			}
			if (m_pagedArenaCache.TryReuseRange(rangeKey, rangeRevision))
			{
				return;
			}
			rangeInstances.Clear(false);
			rangeStableKeys.Clear(false);
			rangeMaterialVersionRuns.Clear(false);

			auto addArenaShadowInstance =
				[&](const RHIMeshPtr& mesh, const glm::mat4& model, size_t renderQueueTag,
					float baseColorAlpha, uint32_t baseColorSampler, float alphaCutoff,
					const ShaderSetPtr& customDepthShader,
					const RHIMaterialPtr& customDepthMaterial,
					const RHIMaterialVersionPtr& customDepthMaterialVersion, uint64_t stableKey)
			{
				if (renderQueueTag != opaqueQueueTag && renderQueueTag != maskedQueueTag)
				{
					return;
				}
				if (!mesh)
				{
					payloadComplete[arenaPayloadIndex] = false;
					return;
				}

				const bool bSkinned =
					proxy.GetSkeletonOffset() != (std::numeric_limits<uint32_t>::max)() &&
					mesh->m_vertexDescription->HasAttribute(
						RHIVertexDescription::DefaultBoneIdsBinding) &&
					mesh->m_vertexDescription->HasAttribute(
						RHIVertexDescription::DefaultBoneWeightsBinding);
				const bool bMasked = renderQueueTag == maskedQueueTag;
				auto depthMaterial = SelectShadowMaterial(
					mesh->m_vertexDescription, shadowPass.m_shadowType, bSkinned, bMasked,
					customDepthShader, customDepthMaterial, customDepthMaterialVersion, sceneView.m_frame);
				if (!depthMaterial || !preparedMaterials.Get(depthMaterial).m_bHasGraphicsShaders)
				{
					payloadComplete[arenaPayloadIndex] = false;
					return;
				}

				// Shadow materials are immutable derivatives of the exact source
				// version resolved for this submission.
				RHIBatch batch = preparedMaterials.MakeBatch(depthMaterial, mesh);
				auto data = MakeInstanceData(model, *mesh, preparedMaterials.Get(depthMaterial).m_materialInstance,
					bSkinned ? proxy.GetSkeletonOffset() : (std::numeric_limits<uint32_t>::max)(),
					baseColorAlpha, baseColorSampler, alphaCutoff);
				rangeInstances.Add(std::move(data));
				rangeStableKeys.Add(stableKey);
				AppendPackedDrawArenaMaterialVersion(
					rangeMaterialVersionRuns, batch.m_materialVersion);
			};

			for (size_t shadowMeshIndex = 0u; shadowMeshIndex < source->m_meshes.Num();
				++shadowMeshIndex)
			{
				const auto& shadowMesh = source->m_meshes[shadowMeshIndex];
				addArenaShadowInstance(shadowMesh.m_mesh, proxy.ResolveMeshWorldMatrix(shadowMesh),
					shadowMesh.m_renderQueueTag, shadowMesh.m_baseColorFactor.a,
					shadowMesh.m_baseColorSampler, shadowMesh.m_alphaCutoff,
					shadowMesh.m_customDepthShader, shadowMesh.m_customDepthMaterial,
					shadowMesh.m_customDepthMaterial
						? shadowMesh.m_customDepthMaterial->GetVersionForSubmission(
							  materialSubmissionId)
						: RHIMaterialVersionPtr{},
					BuildPackedDrawStableKey(proxy.m_handle, proxy.GetProducerKey(), 0u,
						static_cast<uint32_t>(shadowMeshIndex), 0u));
			}

			const auto& topology = proxy.m_resource->m_proxy;
			for (size_t groupIndex = 0u; groupIndex < topology.m_instancedGroups.Num();
				++groupIndex)
			{
				const auto& group = topology.m_instancedGroups[groupIndex];
				if (!group.m_bCastShadows)
				{
					continue;
				}
				for (size_t meshIndex = 0u; meshIndex < group.m_meshes.Num(); ++meshIndex)
				{
					const size_t renderQueueTag = meshIndex < group.m_renderQueueTags.Num()
						? group.m_renderQueueTags[meshIndex]
						: 0u;
					ShaderSetPtr customDepthShader;
					RHIMaterialPtr customDepthMaterial;
					if (meshIndex < group.m_sourceMaterialShaders.Num() &&
						group.m_sourceMaterialShaders[meshIndex] &&
						meshIndex < group.m_materials.Num() && group.m_materials[meshIndex] &&
						group.m_materials[meshIndex]
							->GetRenderState()
							.IsRequiredCustomDepthShader())
					{
						customDepthShader = group.m_sourceMaterialShaders[meshIndex];
						customDepthMaterial = group.m_materials[meshIndex];
					}
					for (size_t instanceIndex = 0u;
						instanceIndex < group.m_instanceTransforms.Num(); ++instanceIndex)
					{
						addArenaShadowInstance(group.m_meshes[meshIndex],
							proxy.ResolveInstancedMeshWorldMatrix(group, instanceIndex, meshIndex),
							renderQueueTag,
							meshIndex < group.m_baseColorFactors.Num()
								? group.m_baseColorFactors[meshIndex].a
								: 1.0f,
							meshIndex < group.m_baseColorSamplers.Num()
								? group.m_baseColorSamplers[meshIndex]
								: 0u,
							meshIndex < group.m_alphaCutoffs.Num() ? group.m_alphaCutoffs[meshIndex]
																   : 0.5f,
							customDepthShader, customDepthMaterial,
							customDepthMaterial
								? customDepthMaterial->GetVersionForSubmission(materialSubmissionId)
								: RHIMaterialVersionPtr{},
							BuildPackedDrawStableKey(proxy.m_handle, proxy.GetProducerKey(),
								static_cast<uint32_t>(groupIndex + 1u),
								static_cast<uint32_t>(meshIndex),
								static_cast<uint32_t>(instanceIndex)));
					}
				}
			}
			if (!m_pagedArenaCache.ReplaceRange(rangeKey, rangeRevision, rangeInstances,
					rangeStableKeys, &rangeMaterialVersionRuns))
			{
				payloadComplete[arenaPayloadIndex] = false;
			}
		};
		for (const auto& proxy : m_arenaChanges.m_updated)
			buildRange(RHIVisibleShadowCaster(proxy.m_handle, *proxy.m_record, *proxy.m_resource));
		m_arenaChanges.Clear();

		auto arenaPayload =
			m_pagedArenaCache.EndUpdate(payloadComplete[arenaPayloadIndex]);
		viewResources.m_packet.UseSharedArenaPayload(mobility, std::move(arenaPayload));
		rangeInstances.Clear(false);
		rangeStableKeys.Clear(false);
		rangeMaterialVersionRuns.Clear(false);
	}
}

void ShadowPrepassNode::BuildVisiblePacket(const RHISceneViewSnapshot& sceneView,
	SubmissionResources& resources, RHIMaterialPreparationCache& preparedMaterials, uint32_t passIndex,
	bool bUsesPagedArenas)
{
	SAILOR_PROFILE_SCOPE("Build visible shadow packet");
	const auto& shadowPass = sceneView.m_shadowMapsToUpdate[passIndex];
	auto& viewResources = *resources.m_activeShadowViews[passIndex];
	auto& payloadComplete = resources.m_shadowPayloadComplete[passIndex];
	const uint64_t materialSubmissionId = sceneView.m_submissionContext->GetSubmissionId();
	const size_t opaqueQueueTag = "Opaque"_h.GetHash();
	const size_t maskedQueueTag = "Masked"_h.GetHash();
	for (const auto& proxy : shadowPass.m_meshList)
	{
		const auto* source = proxy.GetSource();
		if (!source)
		{
			continue;
		}
		const EMobilityType payloadMobility = proxy.GetMobility();
		const size_t payloadIndex = RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(payloadMobility);
		const bool bArenaView = bUsesPagedArenas &&
			(payloadMobility == EMobilityType::Static || payloadMobility == EMobilityType::Stationary);

		for (size_t shadowMeshIndex = 0u; shadowMeshIndex < source->m_meshes.Num(); ++shadowMeshIndex)
		{
			const auto& shadowMesh = source->m_meshes[shadowMeshIndex];
			const auto& mesh = sceneView.ResolveMesh(proxy, shadowMeshIndex);
			if (!mesh)
			{
				if (shadowMesh.m_renderQueueTag == opaqueQueueTag ||
					shadowMesh.m_renderQueueTag == maskedQueueTag)
				{
					payloadComplete[payloadIndex] = false;
				}
				continue;
			}
			const glm::mat4 meshWorldMatrix = proxy.ResolveMeshWorldMatrix(shadowMesh);

			if (shadowMesh.m_maxCameraDistance < (std::numeric_limits<float>::max)())
			{
				Math::AABB worldBounds = mesh->m_bounds;
				worldBounds.Apply(meshWorldMatrix);
				const glm::vec3 cameraPosition(sceneView.m_cameraTransform.m_position);
				const glm::vec3 closestPoint = glm::clamp(cameraPosition, worldBounds.m_min, worldBounds.m_max);
				if (glm::distance(cameraPosition, closestPoint) > shadowMesh.m_maxCameraDistance)
				{
					continue;
				}
			}
			const size_t renderQueueTag = shadowMesh.m_renderQueueTag;
			if (renderQueueTag != opaqueQueueTag && renderQueueTag != maskedQueueTag)
			{
				continue;
			}

			const bool bSkinned = proxy.GetSkeletonOffset() != (std::numeric_limits<uint32_t>::max)() &&
				mesh->m_vertexDescription->HasAttribute(RHI::RHIVertexDescription::DefaultBoneIdsBinding) &&
				mesh->m_vertexDescription->HasAttribute(RHI::RHIVertexDescription::DefaultBoneWeightsBinding);
			const bool bMasked = renderQueueTag == maskedQueueTag;
			auto depthMaterial = SelectShadowMaterial(
				mesh->m_vertexDescription, shadowPass.m_shadowType, bSkinned, bMasked,
				shadowMesh.m_customDepthShader, shadowMesh.m_customDepthMaterial,
				shadowMesh.m_customDepthMaterial ?
					shadowMesh.m_customDepthMaterial->GetVersionForSubmission(materialSubmissionId) :
					RHIMaterialVersionPtr{}, sceneView.m_frame);

			const bool bIsDepthMaterialReady =
				depthMaterial && preparedMaterials.Get(depthMaterial).m_bHasGraphicsShaders;

			if (!bIsDepthMaterialReady)
			{
				payloadComplete[payloadIndex] = false;
				continue;
			}
			RHIBatch batch = preparedMaterials.MakeBatch(depthMaterial, mesh);

			auto data = MakeInstanceData(meshWorldMatrix, *mesh, preparedMaterials.Get(depthMaterial).m_materialInstance,
				bSkinned ? proxy.GetSkeletonOffset() : (std::numeric_limits<uint32_t>::max)(),
				shadowMesh.m_baseColorFactor.a, shadowMesh.m_baseColorSampler, shadowMesh.m_alphaCutoff);

			if (bMasked || depthMaterial->GetRenderState().IsRequiredCustomDepthShader())
			{
				uint32_t supportedMeshesPerBatch = (std::numeric_limits<uint32_t>::max)();
				bool bCurrentTextureBindings = false;
	#if defined(__APPLE__)
				batch.m_textureBindings = Framegraph::Details::GetTextureBindingSet(m_textureBindingCache,
					shadowMesh.m_materialTextureSamplers, sceneView.m_frame, supportedMeshesPerBatch,
					bCurrentTextureBindings);
	#else
				batch.m_textureBindings = App::GetSubmodule<TextureImporter>()->GetTextureSamplersBindingSet();
				bCurrentTextureBindings = batch.m_textureBindings.IsValid();
	#endif
				batch.m_supportedMeshesPerBatch = supportedMeshesPerBatch;
				if (!bCurrentTextureBindings)
				{
					payloadComplete[payloadIndex] = false;
				}
				if (!batch.m_textureBindings)
				{
					continue;
				}
			}
			const uint64_t stableKey = BuildPackedDrawStableKey(
				proxy.m_handle, proxy.GetProducerKey(), 0u, static_cast<uint32_t>(shadowMeshIndex), 0u);
			if (bArenaView)
			{
				if (!viewResources.m_packet.AddArenaView(std::move(batch), mesh,
						BuildPackedDrawRangeKey(proxy.m_handle, proxy.GetProducerKey(), proxy.m_resource),
						stableKey, payloadMobility))
				{
					payloadComplete[payloadIndex] = false;
				}
				continue;
			}

			viewResources.m_packet.Add(std::move(batch), mesh, data, stableKey, payloadMobility);
		}

		const auto* topology = &proxy.m_resource->m_proxy;

		for (size_t groupIndex = 0u; groupIndex < topology->m_instancedGroups.Num(); ++groupIndex)
		{
			const auto& group = topology->m_instancedGroups[groupIndex];
			if (!group.m_bCastShadows)
			{
				continue;
			}

			for (size_t meshIndex = 0u; meshIndex < group.m_meshes.Num(); ++meshIndex)
			{
				const size_t renderQueueTag =
					meshIndex < group.m_renderQueueTags.Num() ? group.m_renderQueueTags[meshIndex] : 0u;
				if (renderQueueTag != opaqueQueueTag && renderQueueTag != maskedQueueTag)
				{
					continue;
				}

				const auto& sourceMesh = group.m_meshes[meshIndex];
				if (!sourceMesh)
				{
					payloadComplete[payloadIndex] = false;
					continue;
				}

				const bool bSkinned = proxy.GetSkeletonOffset() != (std::numeric_limits<uint32_t>::max)() &&
					sourceMesh->m_vertexDescription->HasAttribute(
						RHI::RHIVertexDescription::DefaultBoneIdsBinding) &&
					sourceMesh->m_vertexDescription->HasAttribute(
						RHI::RHIVertexDescription::DefaultBoneWeightsBinding);
				const bool bMasked = renderQueueTag == maskedQueueTag;
				ShaderSetPtr customDepthShader;
				RHIMaterialPtr customDepthMaterial;
				if (meshIndex < group.m_sourceMaterialShaders.Num() &&
					group.m_sourceMaterialShaders[meshIndex] && meshIndex < group.m_materials.Num() &&
					group.m_materials[meshIndex] &&
					group.m_materials[meshIndex]->GetRenderState().IsRequiredCustomDepthShader())
				{
					customDepthShader = group.m_sourceMaterialShaders[meshIndex];
					customDepthMaterial = group.m_materials[meshIndex];
				}
				auto depthMaterial = SelectShadowMaterial(
					sourceMesh->m_vertexDescription, shadowPass.m_shadowType, bSkinned, bMasked,
					customDepthShader, customDepthMaterial,
					customDepthMaterial ? customDepthMaterial->GetVersionForSubmission(materialSubmissionId) :
						RHIMaterialVersionPtr{}, sceneView.m_frame);

				const bool bIsDepthMaterialReady =
					depthMaterial && preparedMaterials.Get(depthMaterial).m_bHasGraphicsShaders;
				if (!bIsDepthMaterialReady)
				{
					payloadComplete[payloadIndex] = false;
					continue;
				}

				RHIBatch batchTemplate = preparedMaterials.MakeBatch(depthMaterial, sourceMesh);
				const uint32_t materialInstance =
					preparedMaterials.Get(depthMaterial).m_materialInstance;
				if (bMasked || depthMaterial->GetRenderState().IsRequiredCustomDepthShader())
				{
					uint32_t supportedMeshesPerBatch = (std::numeric_limits<uint32_t>::max)();
					bool bCurrentTextureBindings = false;
	#if defined(__APPLE__)
					const auto& requestedTextures = meshIndex < group.m_materialTextureSamplers.Num()
						? group.m_materialTextureSamplers[meshIndex]
						: Framegraph::Details::GetDefaultRequestedTextures();
					batchTemplate.m_textureBindings = Framegraph::Details::GetTextureBindingSet(
						m_textureBindingCache, requestedTextures, sceneView.m_frame, supportedMeshesPerBatch,
						bCurrentTextureBindings);
	#else
					batchTemplate.m_textureBindings =
						App::GetSubmodule<TextureImporter>()->GetTextureSamplersBindingSet();
					bCurrentTextureBindings = batchTemplate.m_textureBindings.IsValid();
	#endif
					batchTemplate.m_supportedMeshesPerBatch = supportedMeshesPerBatch;
					if (!bCurrentTextureBindings)
					{
						payloadComplete[payloadIndex] = false;
					}
					if (!batchTemplate.m_textureBindings)
					{
						continue;
					}
				}

				const float baseColorAlpha = meshIndex < group.m_baseColorFactors.Num()
					? group.m_baseColorFactors[meshIndex].a
					: 1.0f;
				const uint32_t baseColorSampler =
					meshIndex < group.m_baseColorSamplers.Num() ? group.m_baseColorSamplers[meshIndex] : 0u;
				const float alphaCutoff =
					meshIndex < group.m_alphaCutoffs.Num() ? group.m_alphaCutoffs[meshIndex] : 0.5f;

				for (size_t instanceIndex = 0u; instanceIndex < group.m_instanceTransforms.Num();
					++instanceIndex)
				{
					if (!proxy.IsInstancedMeshWithinDistance(group, instanceIndex, meshIndex,
							glm::vec3(sceneView.m_cameraTransform.m_position), group.m_maxShadowDistance))
					{
						continue;
					}
					const auto& mesh =
						sceneView.ResolveInstancedMesh(proxy, groupIndex, instanceIndex, meshIndex);
					if (!mesh)
					{
						payloadComplete[payloadIndex] = false;
						continue;
					}
					const uint64_t stableKey = BuildPackedDrawStableKey(proxy.m_handle, proxy.GetProducerKey(),
						static_cast<uint32_t>(groupIndex + 1u), static_cast<uint32_t>(meshIndex),
						static_cast<uint32_t>(instanceIndex));
					const glm::mat4 meshWorldMatrix =
						proxy.ResolveInstancedMeshWorldMatrix(group, instanceIndex, meshIndex);
					RHIBatch batch = batchTemplate;
					batch.m_mesh = mesh;
					if (bArenaView)
					{
						if (!viewResources.m_packet.AddArenaView(std::move(batch), mesh,
								BuildPackedDrawRangeKey(
									proxy.m_handle, proxy.GetProducerKey(), proxy.m_resource),
								stableKey, payloadMobility))
						{
							payloadComplete[payloadIndex] = false;
						}
						continue;
					}

					auto data = MakeInstanceData(meshWorldMatrix, *mesh, materialInstance,
						bSkinned ? proxy.GetSkeletonOffset() : (std::numeric_limits<uint32_t>::max)(),
						baseColorAlpha, baseColorSampler, alphaCutoff);

					viewResources.m_packet.Add(std::move(batch), mesh, data, stableKey, payloadMobility);
				}
			}
		}
	}
}

void ShadowPrepassNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	m_drawCallStats = {};
	if (!sceneView.m_submissionContext)
	{
		return;
	}

	auto submissionResources = sceneView.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(
		this,
		sceneView.m_cameraIndex,
		0u);
	auto& blurShaderBindings = submissionResources->m_blurShaderBindings;
	auto& renderPassColorAttachments =
		submissionResources->m_renderPassColorAttachments;
	auto& blurDrawBindingSets = submissionResources->m_blurDrawBindingSets;

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	const auto requiresBlur = [](const RHIUpdateShadowMapCommand& pass)
	{
		return pass.m_shadowType == EShadowType::EVSM &&
			(pass.m_blurRadius.x > 0.1f || pass.m_blurRadius.y > 0.1f);
	};
	const bool bHasBlur = std::any_of(sceneView.m_shadowMapsToUpdate.begin(), sceneView.m_shadowMapsToUpdate.end(), requiresBlur);
	if (bHasBlur && !m_pBlurShaderBindings)
	{
		auto shaderFileId = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/Blur.shader");
		const bool bVerticalReady = m_pBlurVerticalShader ||
			App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderFileId->GetFileId(), m_pBlurVerticalShader, { "VERTICAL", "EVSM" });
		const bool bHorizontalReady = m_pBlurHorizontalShader ||
			App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderFileId->GetFileId(), m_pBlurHorizontalShader, { "HORIZONTAL", "EVSM" });

		if (bVerticalReady && bHorizontalReady)
		{
			auto candidateBindings = driver->CreateShaderBindings();
			if (driver->FillShadersLayout(candidateBindings, { m_pBlurVerticalShader->GetDebugVertexShaderRHI(), m_pBlurVerticalShader->GetDebugFragmentShaderRHI() }, 1) &&
				driver->AddBufferToShaderBindings(candidateBindings, "data"_h, sizeof(glm::vec4) * 3, 0, RHI::EShaderBindingType::UniformBuffer))
			{
				RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
				RenderState renderState{ false, false, 0.0f, false, ECullMode::Front, EBlendMode::None, EFillMode::Fill, 0, false };
				auto verticalMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pBlurVerticalShader, candidateBindings);
				auto horizontalMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pBlurHorizontalShader, candidateBindings);
				if (verticalMaterial && horizontalMaterial)
				{
					m_pBlurVerticalMaterial = verticalMaterial;
					m_pBlurHorizontalMaterial = horizontalMaterial;
					m_pBlurShaderBindings = candidateBindings;
				}
			}
		}
	}

	if (bHasBlur && !blurShaderBindings && m_pBlurShaderBindings)
	{
		auto candidateBindings = driver->CreateShaderBindings();
		if (driver->FillShadersLayout(candidateBindings, { m_pBlurVerticalShader->GetDebugVertexShaderRHI(), m_pBlurVerticalShader->GetDebugFragmentShaderRHI() }, 1))
		{
			RHIShaderBindingPtr initialBlurDataBinding = driver->AddBufferToShaderBindings(candidateBindings, "data"_h, sizeof(glm::vec4) * 3, 0, RHI::EShaderBindingType::UniformBuffer);
			if (initialBlurDataBinding)
			{
				const float defaultBlurRadius = 3.0f;
				glm::vec4 blurData[] = { {defaultBlurRadius, 0, 0, 0}, {0,0,0,0}, {0,0,0,0} };
				RHI::Renderer::GetDriverCommands()->UpdateShaderBinding(transferCommandList, initialBlurDataBinding, &blurData, sizeof(glm::vec4) * 3);
				blurShaderBindings = candidateBindings;
			}
		}
	}

	if (!sceneView.m_shadowMapsToBlit.IsEmpty())
	{
		commands->BeginDebugRegion(
			commandList,
			"Downsample Local Shadow Tiles"_h,
			DebugContext::Color_CmdTransfer);
		for (const auto& blit : sceneView.m_shadowMapsToBlit)
		{
			if (!blit.m_source || !blit.m_destination ||
				blit.m_sourceArea.z <= 0 || blit.m_sourceArea.w <= 0 ||
				blit.m_destinationArea.z <= 0 || blit.m_destinationArea.w <= 0)
			{
				continue;
			}

			if (blit.m_source == blit.m_destination)
			{
				auto scratch = driver->GetOrAddTemporaryRenderTarget(
					blit.m_source->GetFormat(),
					glm::ivec2(blit.m_destinationArea.z, blit.m_destinationArea.w),
					1);
				if (!scratch) continue;
				commands->ImageMemoryBarrier(
					commandList, blit.m_source, EImageLayout::TransferSrcOptimal);
				commands->ImageMemoryBarrier(
					commandList, scratch, EImageLayout::TransferDstOptimal);
				commands->BlitImage(
					commandList,
					blit.m_source,
					scratch,
					blit.m_sourceArea,
					glm::ivec4(0, 0, scratch->GetExtent().x, scratch->GetExtent().y),
					ETextureFiltration::Nearest);

				commands->ImageMemoryBarrier(
					commandList, scratch, EImageLayout::TransferSrcOptimal);
				commands->ImageMemoryBarrier(
					commandList, blit.m_destination, EImageLayout::TransferDstOptimal);
				commands->BlitImage(
					commandList,
					scratch,
					blit.m_destination,
					glm::ivec4(0, 0, scratch->GetExtent().x, scratch->GetExtent().y),
					blit.m_destinationArea,
					ETextureFiltration::Nearest);
				commands->ImageMemoryBarrier(
					commandList, scratch, EImageLayout::ShaderReadOnlyOptimal);
				driver->ReleaseTemporaryRenderTarget(scratch);
			}
			else
			{
				commands->ImageMemoryBarrier(
					commandList, blit.m_source, EImageLayout::TransferSrcOptimal);
				commands->ImageMemoryBarrier(
					commandList, blit.m_destination, EImageLayout::TransferDstOptimal);
				commands->BlitImage(
					commandList,
					blit.m_source,
					blit.m_destination,
					blit.m_sourceArea,
					blit.m_destinationArea,
					ETextureFiltration::Nearest);
			}

			commands->ImageMemoryBarrier(
				commandList, blit.m_source, EImageLayout::ShaderReadOnlyOptimal);
			commands->ImageMemoryBarrier(
				commandList, blit.m_destination, EImageLayout::ShaderReadOnlyOptimal);
		}
		commands->EndDebugRegion(commandList);
	}
	if (sceneView.m_shadowMapsToUpdate.Num() == 0)
	{
		return;
	}

	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdGraphics);
	{
		const uint32_t NumShadowPasses = (uint32_t)sceneView.m_shadowMapsToUpdate.Num();

		for (uint32_t passIndex = 0u; passIndex < NumShadowPasses; ++passIndex)
		{
			auto& viewResources = *submissionResources->m_activeShadowViews[passIndex];
			const uint32_t numInstances = viewResources.m_packet.GetNumStorageInstances();
			const uint32_t numInstanceIndices = viewResources.m_packet.GetNumDrawInstances();
			if (numInstances == 0u || numInstanceIndices == 0u)
			{
				continue;
			}
			if (!viewResources.m_perInstanceData ||
				viewResources.m_sizePerInstanceData < sizeof(PerInstanceData) * numInstances ||
				viewResources.m_sizeInstanceIndices < sizeof(uint32_t) * numInstanceIndices)
			{
				auto candidateBindings = driver->CreateShaderBindings();
				if (driver->AddSsboToShaderBindings(
					candidateBindings,
					"data"_h,
					sizeof(PerInstanceData),
					numInstances,
					0u) &&
					driver->AddSsboToShaderBindings(
						candidateBindings,
						"indices"_h,
						sizeof(uint32_t),
						numInstanceIndices,
						1u))
				{
					viewResources.m_perInstanceData = candidateBindings;
					viewResources.m_sizePerInstanceData = sizeof(PerInstanceData) * numInstances;
					viewResources.m_sizeInstanceIndices = sizeof(uint32_t) * numInstanceIndices;
				}
			}
		}

		for (uint32_t index = 0; index < sceneView.m_shadowMapsToUpdate.Num(); index++)
		{
			char debugMarker[64];
			sprintf_s(debugMarker, sizeof(debugMarker), "Record Shadow Map Pass %d", index);
			SAILOR_PROFILE_SCOPE("Record Shadow Map Pass");

			const auto& shadowPass = sceneView.m_shadowMapsToUpdate[index];
			const glm::ivec2 shadowExtent = shadowPass.m_shadowMap->GetExtent();
			const bool bUsesAtlasTile = shadowPass.m_renderArea.z > 0 && shadowPass.m_renderArea.w > 0;
			const glm::ivec4 renderArea = bUsesAtlasTile ?
				shadowPass.m_renderArea :
				glm::ivec4(0, 0, shadowExtent.x, shadowExtent.y);

			RHI::RHIRenderTargetPtr depthAttachment = driver->GetOrAddTemporaryRenderTarget(
				driver->GetDepthBuffer()->GetFormat(),
				shadowExtent,
				1);
			const bool bRequiresBlur = requiresBlur(shadowPass);
			RHI::RHIRenderTargetPtr blurAttachment;
			if (depthAttachment && bRequiresBlur && blurShaderBindings)
			{
				blurAttachment = driver->GetOrAddTemporaryRenderTarget(shadowPass.m_shadowMap->GetFormat(), shadowExtent, 1);
			}
			if (!depthAttachment || (bRequiresBlur && blurShaderBindings && !blurAttachment))
			{
				if (shadowPass.m_payloadCompletionToken) shadowPass.m_payloadCompletionToken->Complete(false);
				for (uint32_t dependency : shadowPass.m_internalCommandsList)
				{
					if (auto token = sceneView.m_shadowMapsToUpdate[dependency].m_payloadCompletionToken) token->Complete(false);
				}
				if (depthAttachment) driver->ReleaseTemporaryRenderTarget(depthAttachment);
				continue;
			}

			commands->BeginDebugRegion(commandList, debugMarker, DebugContext::Color_CmdGraphics);
			{
				const auto depthAttachmentLayout = RHI::IsDepthStencilFormat(depthAttachment->GetFormat()) ? EImageLayout::DepthStencilAttachmentOptimal : EImageLayout::DepthAttachmentOptimal;

				commands->ImageMemoryBarrier(commandList, shadowPass.m_shadowMap, EImageLayout::ColorAttachmentOptimal);
				commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachmentLayout);

				// The shadow target stores either raw PCF depth or EVSM moments. With
				// reverse Z, an empty texel represents depth zero. EVSM must encode
				// that value instead of clearing all four moments to zero.
				const glm::vec4 shadowClearValue = shadowPass.m_shadowType == EShadowType::EVSM ?
					glm::vec4(1.0f, 1.0f, -1.0f, 1.0f) :
					glm::vec4(0.0f);

				renderPassColorAttachments.Clear(false);
				renderPassColorAttachments.Add(shadowPass.m_shadowMap);
				commands->BeginRenderPass(commandList,
					renderPassColorAttachments,
					depthAttachment,
					glm::vec4(renderArea),
					glm::ivec2(0, 0),
					!bUsesAtlasTile,
					shadowClearValue,
					0.0f,
					false,
					true);
				if (bUsesAtlasTile)
				{
					commands->ClearAttachments(commandList, renderArea, shadowClearValue, 0.0f);
				}

				auto recordShadowPacket = [&](uint32_t packetIndex)
				{
					if (packetIndex >= submissionResources->m_activeShadowViews.Num())
					{
						return false;
					}
					auto& viewResources = *submissionResources->m_activeShadowViews[packetIndex];
					DrawCallStats stats{};
					if (!viewResources.m_packet.GetGroups().IsEmpty() && viewResources.m_perInstanceData &&
						viewResources.m_sizePerInstanceData >= sizeof(PerInstanceData) * viewResources.m_packet.GetNumStorageInstances() &&
						viewResources.m_sizeInstanceIndices >= sizeof(uint32_t) * viewResources.m_packet.GetNumDrawInstances())
					{
						const auto prepareShadowMaterial = [&](
							const RHIBatch& batch,
							TVector<RHIShaderBindingSetPtr>& sets)
						{
							commands->PushConstants(commandList, batch.m_material,
								sizeof(shadowPass.m_lightMatrix), &shadowPass.m_lightMatrix);
							if (batch.m_material->GetRenderState().IsRequiredCustomDepthShader())
							{
								sets.Add(sceneView.m_frameBindings);
								sets.Add(sceneView.m_rhiLightsData);
								sets.Add(viewResources.m_perInstanceData);
								sets.Add(batch.GetMaterialBindings());
								sets.Add(batch.m_textureBindings);
							}
							else
							{
								sets.Add(sceneView.m_frameBindings);
								sets.Add(viewResources.m_perInstanceData);
								if (batch.m_textureBindings)
								{
									sets.Add(batch.m_textureBindings);
								}
							}
							const bool bSkinned =
								batch.m_mesh->m_vertexDescription->HasAttribute(RHIVertexDescription::DefaultBoneIdsBinding) &&
								batch.m_mesh->m_vertexDescription->HasAttribute(RHIVertexDescription::DefaultBoneWeightsBinding);
							if (bSkinned && sceneView.m_boneMatrices)
							{
								sets.Add(sceneView.m_boneMatrices);
							}
						};

						stats = RHIRecordPackedDrawPacket(
							viewResources.m_packet,
							commandList,
							transferCommandList,
							prepareShadowMaterial,
							viewResources.m_perInstanceData,
							viewResources.m_indirectBuffer,
							glm::ivec4(renderArea.x, renderArea.y + renderArea.w, renderArea.z, -renderArea.w),
							glm::uvec4(renderArea),
							glm::vec2(0.0f, 1.0f));
					}
					m_drawCallStats += stats;
					const bool bComplete = stats.m_numInstances == viewResources.m_packet.GetNumDrawInstances();
					const auto& token = sceneView.m_shadowMapsToUpdate[packetIndex].m_payloadCompletionToken;
					if (!bComplete && token)
					{
						token->Complete(false);
					}
					return bComplete && (!token || token->IsSuccessful());
				};

				bool bShadowMapComplete = recordShadowPacket(index);
				for (uint32_t dependencyPass : shadowPass.m_internalCommandsList)
				{
					bShadowMapComplete &= recordShadowPacket(dependencyPass);
				}

				commands->EndRenderPass(commandList);

				if (bRequiresBlur && blurShaderBindings)
				{
					auto fullscreenMesh = frameGraph->GetFullscreenNdcQuad();
					const uint32_t firstIndex = (uint32_t)fullscreenMesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
					const uint32_t vertexOffset = (uint32_t)fullscreenMesh->m_vertexBuffer->GetOffset() / (uint32_t)fullscreenMesh->m_vertexDescription->GetVertexStride();
					commands->BindVertexBuffer(commandList, fullscreenMesh->m_vertexBuffer, 0);
					commands->BindIndexBuffer(commandList, fullscreenMesh->m_indexBuffer, 0);
					RHIShaderBindingPtr blurDataBinding = blurShaderBindings->GetOrAddShaderBinding("data"_h);
					// The flight reuses this UBO for every shadow map.
					commands->MemoryBarrier(commandList, static_cast<EAccessFlags>(EAccessBit::UniformRead_Bit), static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit));
					commands->UpdateShaderBinding(commandList, blurDataBinding, &shadowPass.m_blurRadius, sizeof(glm::vec2));
					commands->MemoryBarrier(commandList, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit), static_cast<EAccessFlags>(EAccessBit::UniformRead_Bit));
					bool bBlurComplete = false;

					// Blur Horizontal
					commands->BeginDebugRegion(commandList, "Blur Horizontal"_h, DebugContext::Color_CmdPostProcess);
					{
						bBlurComplete = static_cast<bool>(driver->AddSamplerToShaderBindings(
							blurShaderBindings,
							"colorSampler"_h,
							shadowPass.m_shadowMap,
							1));

						commands->ImageMemoryBarrier(commandList, shadowPass.m_shadowMap, EImageLayout::ShaderReadOnlyOptimal);
						commands->ImageMemoryBarrier(commandList, blurAttachment, EImageLayout::ColorAttachmentOptimal);

						renderPassColorAttachments.Clear(false);
						renderPassColorAttachments.Add(blurAttachment);
						commands->BeginRenderPass(commandList,
							renderPassColorAttachments,
							nullptr,
							glm::vec4(0, 0, shadowPass.m_shadowMap->GetExtent().x, shadowPass.m_shadowMap->GetExtent().y),
							glm::ivec2(0, 0),
							false,
							glm::vec4(0.0f),
							0.0f,
							false);

						commands->BindMaterial(commandList, m_pBlurHorizontalMaterial);
						blurDrawBindingSets.Clear(false);
						blurDrawBindingSets.Add(sceneView.m_frameBindings);
						blurDrawBindingSets.Add(blurShaderBindings);
						bBlurComplete = bBlurComplete && commands->BindShaderBindings(
							commandList,
							m_pBlurHorizontalMaterial,
							blurDrawBindingSets);
						if (bBlurComplete)
						{
							commands->SetViewport(commandList,
								0, 0,
								(float)shadowPass.m_shadowMap->GetExtent().x, (float)shadowPass.m_shadowMap->GetExtent().y,
								glm::vec2(0, 0),
								glm::vec2(shadowPass.m_shadowMap->GetExtent().x, shadowPass.m_shadowMap->GetExtent().y),
								0, 1.0f);

							commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
							m_drawCallStats.m_numBatches++;
							m_drawCallStats.m_numInstances++;
						}
						commands->EndRenderPass(commandList);

						commands->ImageMemoryBarrier(commandList, shadowPass.m_shadowMap, EImageLayout::ColorAttachmentOptimal);
						commands->ImageMemoryBarrier(commandList, blurAttachment, EImageLayout::ShaderReadOnlyOptimal);
					}
					commands->EndDebugRegion(commandList);

					if (bBlurComplete)
					{
						// Blur Vertical
						commands->BeginDebugRegion(commandList, "Blur Vertical"_h, DebugContext::Color_CmdPostProcess);
						bBlurComplete = static_cast<bool>(driver->AddSamplerToShaderBindings(
							blurShaderBindings,
							"colorSampler"_h,
							blurAttachment,
							1));

						renderPassColorAttachments.Clear(false);
						renderPassColorAttachments.Add(shadowPass.m_shadowMap);
						commands->BeginRenderPass(commandList,
							renderPassColorAttachments,
							nullptr,
							glm::vec4(0, 0, shadowPass.m_shadowMap->GetExtent().x, shadowPass.m_shadowMap->GetExtent().y),
							glm::ivec2(0, 0),
							false,
							glm::vec4(0.0f),
							0.0f,
							false);

						commands->BindMaterial(commandList, m_pBlurVerticalMaterial);
						blurDrawBindingSets.Clear(false);
						blurDrawBindingSets.Add(sceneView.m_frameBindings);
						blurDrawBindingSets.Add(blurShaderBindings);
						bBlurComplete = bBlurComplete && commands->BindShaderBindings(
							commandList,
							m_pBlurVerticalMaterial,
							blurDrawBindingSets);
						if (bBlurComplete)
						{
							commands->SetViewport(commandList,
								0, 0,
								(float)shadowPass.m_shadowMap->GetExtent().x, (float)shadowPass.m_shadowMap->GetExtent().y,
								glm::vec2(0, 0),
								glm::vec2(shadowPass.m_shadowMap->GetExtent().x, shadowPass.m_shadowMap->GetExtent().y),
								0, 1.0f);

							commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
							m_drawCallStats.m_numBatches++;
							m_drawCallStats.m_numInstances++;
						}
						commands->EndRenderPass(commandList);
						commands->EndDebugRegion(commandList);
					}

					bShadowMapComplete &= bBlurComplete;
					driver->ReleaseTemporaryRenderTarget(blurAttachment);
				}
				else if (bRequiresBlur)
				{
					bShadowMapComplete = false;
				}
				if (!bShadowMapComplete && shadowPass.m_payloadCompletionToken)
				{
					shadowPass.m_payloadCompletionToken->Complete(false);
				}

				// Lighting samples every completed shadow map later in the same
				// graphics command list. Publish the color writes explicitly for both
				// the direct PCF path and the final EVSM blur pass.
				commands->ImageMemoryBarrier(commandList, shadowPass.m_shadowMap, EImageLayout::ShaderReadOnlyOptimal);

				driver->ReleaseTemporaryRenderTarget(depthAttachment);

			}
			commands->EndDebugRegion(commandList);

		}
	}
	commands->EndDebugRegion(commandList);
}

void ShadowPrepassNode::Clear()
{
	m_shadowMaterials_Pcf.Clear();
	m_shadowMaterials_Evsm.Clear();
	m_skinnedShadowMaterials_Pcf.Clear();
	m_skinnedShadowMaterials_Evsm.Clear();
	m_maskedShadowMaterials_Pcf.Clear();
	m_maskedShadowMaterials_Evsm.Clear();
	m_skinnedMaskedShadowMaterials_Pcf.Clear();
	m_skinnedMaskedShadowMaterials_Evsm.Clear();
	m_customShadowMaterials.Clear();
	m_textureBindingCache.Clear();
	m_pagedArenaCache.Clear();
}

glm::mat4 ShadowPrepassNode::CalculateLightProjectionMatrix(const glm::mat4& lightView, const glm::mat4& cameraWorld, float aspect, float fovY, float zNear, float zFar, float zMult, glm::ivec2 shadowMapResolution, float zSourceExtension)
{
	SAILOR_PROFILE_FUNCTION();

	Math::Frustum cameraFrustum{};
	cameraFrustum.ExtractFrustumPlanes(cameraWorld, aspect, fovY, zNear, zFar);
	return cameraFrustum.CalculateOrthoMatrixByView(lightView, zMult, shadowMapResolution, zSourceExtension);
}

void ShadowPrepassNode::CalculateLightProjectionForCascades(
	const glm::mat4& lightView,
	const glm::mat4& cameraWorld,
	float aspect,
	float fovY,
	float cameraNearPlane,
	float cameraFarPlane,
	TVector<glm::mat4>& outMatrices)
{
	SAILOR_PROFILE_FUNCTION();
	const auto& graphicsProfile = App::GetActiveGraphicsSettings();
	const float shadowFarPlane = (std::min)(cameraFarPlane, graphicsProfile.m_shadowDistance);
	const uint32_t activeCascadeCount = (std::clamp)(
		graphicsProfile.m_shadowCascadeCount,
		1u,
		LightingECS::NumCascades);
	outMatrices.Clear(false);
	outMatrices.Reserve(activeCascadeCount);
	for (uint32_t i = 0; i < activeCascadeCount; ++i)
	{
		const float cascadeFar = shadowFarPlane *
			LightingECS::GetShadowCascadeLevel(i, activeCascadeCount);
		float cascadeNear = cameraNearPlane;
		if (i > 0)
		{
			const float previousSplit = shadowFarPlane *
				LightingECS::GetShadowCascadeLevel(i - 1u, activeCascadeCount);
			const float previousNear = i > 1 ?
				shadowFarPlane * LightingECS::GetShadowCascadeLevel(
					i - 2u, activeCascadeCount) : cameraNearPlane;
			const float overlap = (previousSplit - previousNear) * LightingECS::ShadowCascadeBlendFraction;
			cascadeNear = (std::max)(cameraNearPlane, previousSplit - overlap);
		}

		outMatrices.Add(CalculateLightProjectionMatrix(lightView, cameraWorld, aspect, fovY,
			cascadeNear,
			cascadeFar,
			10.0f,
			glm::ivec2(graphicsProfile.GetShadowCascadeResolution(i)),
			LightingECS::ShadowCasterDepthExtension));
	}
}
