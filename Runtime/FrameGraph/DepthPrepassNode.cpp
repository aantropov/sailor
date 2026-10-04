#include "DepthPrepassNode.h"
#include "Core/StringHash.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "RHI/Types.h"
#include "RHI/Batch.hpp"
#include "RHI/MaterialPreparationCache.h"
#include "RHI/VertexDescription.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/AssetRegistry.h"

#include <limits>

using namespace Sailor;
using namespace Sailor::RHI;

#ifndef _SAILOR_IMPORT_
const char* DepthPrepassNode::m_name = "DepthPrepass";
#endif

namespace
{
	struct DepthMaterialData
	{
		uint32_t m_materialInstance = 0;
		uint32_t m_alphaCutoffBits = 0;
	};

	DepthMaterialData GetDepthMaterialData(const RenderState& sourceState, uint32_t materialInstance,
		uint32_t baseColorSampler, float baseColorAlpha, float alphaCutoff)
	{
		DepthMaterialData data;
		if (sourceState.IsRequiredCustomDepthShader())
		{
			data.m_materialInstance = materialInstance;
		}
		else if (sourceState.GetTag() == "Masked"_h.GetHash())
		{
			data.m_materialInstance = baseColorSampler;
			const float effectiveAlphaCutoff = baseColorAlpha > 0.000001f ? alphaCutoff / baseColorAlpha : 2.0f;
			data.m_alphaCutoffBits = glm::floatBitsToUint(effectiveAlphaCutoff);
		}
		return data;
	}

	DepthPrepassNode::PerInstanceData MakeInstanceData(
		const glm::mat4& model, const RHIMesh& mesh, uint32_t skeletonOffset, const DepthMaterialData& material)
	{
		DepthPrepassNode::PerInstanceData data;
		data.model = model;
		data.sphereBounds = mesh.m_bounds.ToSphere().GetVec4();
		data.skeletonOffset = skeletonOffset;
		data.materialInstance = material.m_materialInstance;
		data.padding = material.m_alphaCutoffBits;
		return data;
	}

	DepthPrepassNode::CustomPerInstanceData MakeCustomInstanceData(
		const DepthPrepassNode::PerInstanceData& source, const RHIMesh& mesh)
	{
		DepthPrepassNode::CustomPerInstanceData data;
		data.model = source.model;
		data.sphereBounds = source.sphereBounds;
		data.materialInstance = source.materialInstance;
		data.skeletonOffset = source.skeletonOffset;
		data.padding = source.padding;
		data.bakedVolumeScale = vec4(mesh.m_bakedVolumeScale, 1.0f);
		return data;
	}
}

RHI::RHIMaterialPtr DepthPrepassNode::GetOrAddDepthMaterial(
	const RHI::RHIMaterialPtr& source,
	RHI::RHIVertexDescriptionPtr vertexDescription,
	bool bSkinned)
{
	const auto& state = source->GetRenderState();
	if (state.IsRequiredCustomDepthShader())
	{
		return source;
	}
	const bool bMasked = state.GetTag() == "Masked"_h.GetHash();
	const auto cullMode = state.GetCullMode();
	auto& materials = bMasked ?
		(bSkinned ? m_skinnedMaskedDepthOnlyMaterials : m_maskedDepthOnlyMaterials) :
		(bSkinned ? m_skinnedDepthOnlyMaterials : m_depthOnlyMaterials);
	const DepthMaterialKey key{ vertexDescription->GetVertexAttributeBits(), cullMode };
	auto& material = materials[key];

	if (!material)
	{
		auto shaderFileId = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/DepthOnly.shader");
		ShaderSetPtr pShader;
		TVector<std::string> defines;
		if (bSkinned)
		{
			defines.Add("SKINNING");
		}
		if (bMasked)
		{
			defines.Add("MASKED");
		}

		if (App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderFileId->GetFileId(), pShader, defines) && pShader->IsReady())
		{
			RenderState renderState = RHI::RenderState(true, true, 0.0f, false, cullMode, EBlendMode::None, EFillMode::Fill, "DepthOnly"_h.GetHash(), true);
			material = RHI::Renderer::GetDriver()->CreateMaterial(vertexDescription, RHI::EPrimitiveTopology::TriangleList, renderState, pShader);
		}
	}

	return material;
}

RHI::ESortingOrder DepthPrepassNode::GetSortingOrder() const
{
	const std::string& sortOrder = GetString("Sorting");

	if (!sortOrder.empty())
	{
		return magic_enum::enum_cast<RHI::ESortingOrder>(sortOrder).value_or(RHI::ESortingOrder::FrontToBack);
	}

	return RHI::ESortingOrder::FrontToBack;
}

Tasks::TaskPtr<void, void> DepthPrepassNode::Prepare(RHI::RHIFrameGraphPtr frameGraph, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	const std::string QueueTag = GetString("Tag");
	const size_t QueueTagHash = StringHash::Runtime(QueueTag).GetHash();
	std::string virtualizeInstancePayloadsSetting;
	const bool bVirtualizeInstancePayloads =
		!TryGetString("VirtualizeInstancePayloads", virtualizeInstancePayloadsSetting) ||
		virtualizeInstancePayloadsSetting != "false";

	auto res = Tasks::CreateTask("Prepare DepthPrepassNode " + std::to_string(sceneView.m_frame),
		[=, this, holdRhiResources = frameGraph, &syncSharedResources = m_syncSharedResources, &sceneViewSnapshot = sceneView]() mutable {
			if (!sceneViewSnapshot.m_submissionContext)
			{
				return;
			}
			RHIMaterialPreparationCache preparedMaterials(sceneViewSnapshot.m_submissionContext->GetSubmissionId());

			auto submissionResources = sceneViewSnapshot.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(
				this,
				sceneViewSnapshot.m_cameraIndex,
				0u);
			auto& packet = submissionResources->m_packet;
			auto& customPacket = submissionResources->m_customPacket;

			packet.Reset();
			customPacket.Reset();
			syncSharedResources.Lock();
			if (bVirtualizeInstancePayloads)
			{
				BuildStableArenas(sceneViewSnapshot, *submissionResources, preparedMaterials, QueueTagHash);
			}
			Framegraph::Details::EvictTextureBindingCache(m_textureBindingCache, sceneViewSnapshot.m_frame);
			m_pagedArenaCache.Evict(sceneViewSnapshot.m_frame);
			m_customPagedArenaCache.Evict(sceneViewSnapshot.m_frame);
			BuildVisiblePacket(sceneViewSnapshot, *submissionResources, preparedMaterials, QueueTagHash, bVirtualizeInstancePayloads);
			syncSharedResources.Unlock();

			packet.Finalize(false);
			customPacket.Finalize(false);
		}, EThreadType::RHI);

	return res;
}

void DepthPrepassNode::BuildStableArenas(const RHISceneViewSnapshot& sceneView,
	SubmissionResources& resources, RHIMaterialPreparationCache& preparedMaterials, size_t queueTagHash)
{
	SAILOR_PROFILE_SCOPE("Build depth stable arenas");
	auto& packet = resources.m_packet;
	auto& customPacket = resources.m_customPacket;
	const uint64_t materialSubmissionId = sceneView.m_submissionContext->GetSubmissionId();
	const bool bMaskedQueue = queueTagHash == "Masked"_h.GetHash();
	for (const EMobilityType mobility : { EMobilityType::Static, EMobilityType::Stationary })
	{
		const size_t arenaPayloadIndex = TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(mobility);
		size_t payloadRevision = Fnv1aOffsetBasis;
		HashCombine(payloadRevision, arenaPayloadIndex, queueTagHash, bMaskedQueue,
			sceneView.m_submissionContext->GetMaterialRevision(), sceneView.GetMobilityRevision(mobility));
		size_t arenaCacheSlot = Fnv1aOffsetBasis;
		HashCombine(arenaCacheSlot, 0u, arenaPayloadIndex);
		auto packetPayload = m_pagedArenaCache.Find(arenaCacheSlot, payloadRevision, sceneView.m_frame);
		auto customPayload = m_customPagedArenaCache.Find(arenaCacheSlot, payloadRevision, sceneView.m_frame);
		const bool bBuildPacketPayload = !packetPayload;
		const bool bBuildCustomPayload = !customPayload;
		if (packetPayload) packet.UseSharedArenaPayload(mobility, std::move(packetPayload));
		if (customPayload) customPacket.UseSharedArenaPayload(mobility, std::move(customPayload));
		if (!bBuildPacketPayload && !bBuildCustomPayload)
		{
			continue;
		}
		if (bBuildPacketPayload)
		{
			m_pagedArenaCache.BeginUpdate(
				arenaCacheSlot, payloadRevision, sceneView.m_frame);
		}
		if (bBuildCustomPayload)
		{
			m_customPagedArenaCache.BeginUpdate(
				arenaCacheSlot, payloadRevision, sceneView.m_frame);
		}
		auto& rangeInstances = resources.m_arenaRangeInstances;
		auto& rangeStableKeys = resources.m_arenaRangeStableKeys;
		auto& rangeMaterialVersionRuns = resources.m_arenaRangeMaterialVersionRuns;
		auto& customRangeInstances = resources.m_customArenaRangeInstances;
		auto& customRangeStableKeys = resources.m_customArenaRangeStableKeys;
		auto& customRangeMaterialVersionRuns = resources.m_customArenaRangeMaterialVersionRuns;
		bool bPacketPayloadComplete = true;
		bool bCustomPayloadComplete = true;
		bool bBuildPacketRange = false;
		bool bBuildCustomRange = false;
		auto addArenaDepthInstance = [&](const RHIVisibleSceneProxy& proxy,
			const RHIMeshPtr& mesh,
			const RHIMaterialPtr& sourceMaterial,
			const glm::mat4& model,
			uint32_t baseColorSampler,
			const glm::vec4& baseColorFactor,
			float alphaCutoff,
			uint64_t stableKey)
		{
			if (!sourceMaterial || sourceMaterial->GetRenderState().GetTag() != queueTagHash)
			{
				return;
			}
			const bool bRequiredCustomDepth =
				sourceMaterial->GetRenderState().IsRequiredCustomDepthShader();
			if ((bRequiredCustomDepth && !bBuildCustomRange) ||
				(!bRequiredCustomDepth && !bBuildPacketRange))
			{
				return;
			}
			if (!mesh)
			{
				if (bRequiredCustomDepth)
				{
					bCustomPayloadComplete = false;
				}
				else
				{
					bPacketPayloadComplete = false;
				}
				return;
			}

			const bool bSkinned = proxy.GetSkeletonOffset() != (std::numeric_limits<uint32_t>::max)() &&
				mesh->m_vertexDescription->HasAttribute(RHIVertexDescription::DefaultBoneIdsBinding) &&
				mesh->m_vertexDescription->HasAttribute(RHIVertexDescription::DefaultBoneWeightsBinding);
			auto depthMaterial = GetOrAddDepthMaterial(sourceMaterial, mesh->m_vertexDescription, bSkinned);
			const bool bReady = depthMaterial &&
				preparedMaterials.Get(depthMaterial).m_bHasGraphicsShaders &&
				depthMaterial->GetRenderState().IsEnabledZWrite();
			if (!bReady)
			{
				if (bRequiredCustomDepth)
				{
					bCustomPayloadComplete = false;
				}
				else
				{
					bPacketPayloadComplete = false;
				}
				return;
			}

			RHIBatch batch = preparedMaterials.MakeBatch(depthMaterial, mesh);
			const auto materialData = GetDepthMaterialData(sourceMaterial->GetRenderState(),
				bRequiredCustomDepth ? preparedMaterials.Get(depthMaterial).m_materialInstance : 0u,
				baseColorSampler, baseColorFactor.a, alphaCutoff);
			auto data = MakeInstanceData(model, *mesh,
				bSkinned ? proxy.GetSkeletonOffset() : (std::numeric_limits<uint32_t>::max)(), materialData);

			if (bRequiredCustomDepth)
			{
				auto customData = MakeCustomInstanceData(data, *mesh);
				customRangeInstances.Add(std::move(customData));
				customRangeStableKeys.Add(stableKey);
				RHI::AppendPackedDrawArenaMaterialVersion(
					customRangeMaterialVersionRuns, batch.m_materialVersion);
			}
			else
			{
				rangeInstances.Add(std::move(data));
				rangeStableKeys.Add(stableKey);
				RHI::AppendPackedDrawArenaMaterialVersion(
					rangeMaterialVersionRuns, batch.m_materialVersion);
			}
		};

		sceneView.ForEachSceneProxy(mobility,
			[&](const RHIVisibleSceneProxy& proxy)
			{
				const auto* source = proxy.GetSource();
				if (!source)
				{
					return;
				}
				const uint64_t rangeKey =
					BuildPackedDrawRangeKey(proxy.m_handle, source->m_staticMeshEcs, proxy.m_resource);
				size_t rangeRevision =
					proxy.m_resource ? proxy.m_resource->m_depthRevision : proxy.GetContentRevision();
				HashCombine(rangeRevision,
					queueTagHash,
					bMaskedQueue,
					proxy.GetContentRevision(),
					std::hash<glm::mat4>{}(proxy.GetWorldMatrix()),
					proxy.GetSkeletonOffset());
				if (proxy.m_record)
				{
					HashCombine(
						rangeRevision, proxy.m_record->m_materialRevision, proxy.m_record->m_renderFlags);
				}
				auto hashCustomDepthMaterialVersion =
					[&](const RHI::RHIMaterialPtr& material)
					{
						if (!material ||
							material->GetRenderState().GetTag() != queueTagHash ||
							!material->GetRenderState().IsRequiredCustomDepthShader())
						{
							return;
						}

						const auto version =
							material->GetVersionForSubmission(materialSubmissionId);
						HashCombine(
							rangeRevision,
							version ? version->GetVersionId() : 0ull);
					};
				for (const auto& material : source->GetMaterials())
				{
					hashCustomDepthMaterialVersion(material);
				}
				for (const auto& group : source->m_instancedGroups)
				{
					for (const auto& material : group.m_materials)
					{
						hashCustomDepthMaterialVersion(material);
					}
				}
				bBuildPacketRange = bBuildPacketPayload &&
					!m_pagedArenaCache.TryReuseRange(rangeKey, rangeRevision);
				bBuildCustomRange = bBuildCustomPayload &&
					!m_customPagedArenaCache.TryReuseRange(rangeKey, rangeRevision);
				if (!bBuildPacketRange && !bBuildCustomRange)
				{
					return;
				}
				rangeInstances.Clear(false);
				rangeStableKeys.Clear(false);
				rangeMaterialVersionRuns.Clear(false);
				customRangeInstances.Clear(false);
				customRangeStableKeys.Clear(false);
				customRangeMaterialVersionRuns.Clear(false);
				for (size_t meshIndex = 0u; meshIndex < source->m_meshes.Num(); ++meshIndex)
				{
					if (meshIndex >= source->GetMaterials().Num())
					{
						break;
					}
					addArenaDepthInstance(proxy,
						source->m_meshes[meshIndex],
						source->GetMaterials()[meshIndex],
						proxy.ResolveMeshWorldMatrix(meshIndex),
						meshIndex < source->m_baseColorSamplers.Num() ?
							source->m_baseColorSamplers[meshIndex] :
							0u,
						meshIndex < source->m_baseColorFactors.Num() ?
							source->m_baseColorFactors[meshIndex] :
							glm::vec4(1.0f),
						meshIndex < source->m_alphaCutoffs.Num() ? source->m_alphaCutoffs[meshIndex] : 0.5f,
						BuildPackedDrawStableKey(proxy.m_handle,
							source->m_staticMeshEcs,
							0u,
							static_cast<uint32_t>(meshIndex),
							0u));
				}
				for (size_t groupIndex = 0u; groupIndex < source->m_instancedGroups.Num(); ++groupIndex)
				{
					const auto& group = source->m_instancedGroups[groupIndex];
					for (size_t meshIndex = 0u; meshIndex < group.m_meshes.Num(); ++meshIndex)
					{
						if (meshIndex >= group.m_materials.Num())
						{
							break;
						}
						for (size_t instanceIndex = 0u; instanceIndex < group.m_instanceTransforms.Num();
							++instanceIndex)
						{
							addArenaDepthInstance(proxy,
								group.m_meshes[meshIndex],
								group.m_materials[meshIndex],
								proxy.ResolveInstancedMeshWorldMatrix(group, instanceIndex, meshIndex),
								meshIndex < group.m_baseColorSamplers.Num() ?
									group.m_baseColorSamplers[meshIndex] :
									0u,
								meshIndex < group.m_baseColorFactors.Num() ?
									group.m_baseColorFactors[meshIndex] :
									glm::vec4(1.0f),
								meshIndex < group.m_alphaCutoffs.Num() ? group.m_alphaCutoffs[meshIndex] :
																		 0.5f,
								BuildPackedDrawStableKey(proxy.m_handle,
									source->m_staticMeshEcs,
									static_cast<uint32_t>(groupIndex + 1u),
									static_cast<uint32_t>(meshIndex),
									static_cast<uint32_t>(instanceIndex)));
						}
					}
				}
				if (bBuildPacketRange &&
					!m_pagedArenaCache.ReplaceRange(rangeKey,
						rangeRevision,
						rangeInstances,
						rangeStableKeys,
						&rangeMaterialVersionRuns))
				{
					bPacketPayloadComplete = false;
				}
				if (bBuildCustomRange &&
					!m_customPagedArenaCache.ReplaceRange(rangeKey,
						rangeRevision,
						customRangeInstances,
						customRangeStableKeys,
						&customRangeMaterialVersionRuns))
				{
					bCustomPayloadComplete = false;
				}
			});

		if (bBuildPacketPayload)
		{
			auto packetPayload = m_pagedArenaCache.EndUpdate(bPacketPayloadComplete);
			packet.UseSharedArenaPayload(mobility, std::move(packetPayload));
		}
		if (bBuildCustomPayload)
		{
			auto customPayload =
				m_customPagedArenaCache.EndUpdate(bCustomPayloadComplete);
			customPacket.UseSharedArenaPayload(mobility, std::move(customPayload));
		}
		rangeInstances.Clear(false);
		rangeStableKeys.Clear(false);
		rangeMaterialVersionRuns.Clear(false);
		customRangeInstances.Clear(false);
		customRangeStableKeys.Clear(false);
		customRangeMaterialVersionRuns.Clear(false);
	}
}

void DepthPrepassNode::BuildVisiblePacket(const RHISceneViewSnapshot& sceneView,
	SubmissionResources& resources, RHIMaterialPreparationCache& preparedMaterials, size_t queueTagHash,
	bool bUsesPagedArenas)
{
	SAILOR_PROFILE_SCOPE("Build visible depth packet");
	auto& packet = resources.m_packet;
	auto& customPacket = resources.m_customPacket;
	const bool bMaskedQueue = queueTagHash == "Masked"_h.GetHash();
	for (const auto& proxy : sceneView.m_proxies)
	{
		const auto* source = proxy.GetSource();
		if (!source)
		{
			continue;
		}
		const EMobilityType payloadMobility = proxy.GetMobility();
		const bool bArenaView = bUsesPagedArenas &&
			(payloadMobility == EMobilityType::Static || payloadMobility == EMobilityType::Stationary);
		for (size_t i = 0; i < source->m_meshes.Num(); i++)
		{
			const bool bHasMaterial = source->GetMaterials().Num() > i;
			if (!bHasMaterial)
			{
				break;
			}
			if (source->GetMaterials()[i] == nullptr)
			{
				continue;
			}

			const auto& sourceMaterial = source->GetMaterials()[i];
			if (sourceMaterial->GetRenderState().GetTag() != queueTagHash)
			{
				continue;
			}

			const auto& mesh = sceneView.ResolveMesh(proxy, i);
			if (!mesh)
			{
				continue;
			}

			const bool bSkinned =
				proxy.GetSkeletonOffset() != (std::numeric_limits<uint32_t>::max)() &&
				mesh->m_vertexDescription->HasAttribute(RHI::RHIVertexDescription::DefaultBoneIdsBinding) &&
				mesh->m_vertexDescription->HasAttribute(RHI::RHIVertexDescription::DefaultBoneWeightsBinding);
			auto depthMaterial = GetOrAddDepthMaterial(sourceMaterial, mesh->m_vertexDescription, bSkinned);

			const bool bRequiredCustomDepth =
				sourceMaterial->GetRenderState().IsRequiredCustomDepthShader();
			const bool bIsDepthMaterialReady = depthMaterial &&
				preparedMaterials.Get(depthMaterial).m_bHasGraphicsShaders &&
				depthMaterial->GetRenderState().IsEnabledZWrite();

			if (!bIsDepthMaterialReady)
			{
				continue;
			}
			RHIBatch batch = preparedMaterials.MakeBatch(depthMaterial, mesh);

			const auto materialData = GetDepthMaterialData(sourceMaterial->GetRenderState(),
				bRequiredCustomDepth ? preparedMaterials.Get(depthMaterial).m_materialInstance : 0u,
				i < source->m_baseColorSamplers.Num() ? source->m_baseColorSamplers[i] : 0u,
				i < source->m_baseColorFactors.Num() ? source->m_baseColorFactors[i].a : 1.0f,
				i < source->m_alphaCutoffs.Num() ? source->m_alphaCutoffs[i] : 0.5f);
			auto data = MakeInstanceData(proxy.ResolveMeshWorldMatrix(i), *mesh,
				bSkinned ? proxy.GetSkeletonOffset() : (std::numeric_limits<uint32_t>::max)(), materialData);

			if (bRequiredCustomDepth || bMaskedQueue)
			{
				uint32_t supportedMeshesPerBatch = (std::numeric_limits<uint32_t>::max)();
#if defined(__APPLE__)
				bool bCurrentTextureBindings = false;
				const auto& requestedTextures =
					source->m_materialTextureSamplers.Num() > i ?
					source->m_materialTextureSamplers[i] :
					Framegraph::Details::GetDefaultRequestedTextures();
				batch.m_textureBindings = Framegraph::Details::GetTextureBindingSet(
					m_textureBindingCache,
					requestedTextures,
					sceneView.m_frame,
					supportedMeshesPerBatch,
					bCurrentTextureBindings);
#else
				batch.m_textureBindings = App::GetSubmodule<TextureImporter>()->GetTextureSamplersBindingSet();
#endif
				batch.m_supportedMeshesPerBatch = supportedMeshesPerBatch;
				if (!batch.m_textureBindings)
				{
					continue;
				}
			}
			const uint64_t stableKey = BuildPackedDrawStableKey(
				proxy.m_handle,
				source->m_staticMeshEcs,
				0u,
				static_cast<uint32_t>(i),
				0u);
			if (bArenaView)
			{
				const uint64_t rangeKey = BuildPackedDrawRangeKey(
					proxy.m_handle, source->m_staticMeshEcs, proxy.m_resource);
				if (bRequiredCustomDepth)
				{
					customPacket.AddArenaView(
						std::move(batch), mesh, rangeKey, stableKey, payloadMobility);
				}
				else
				{
					packet.AddArenaView(
						std::move(batch), mesh, rangeKey, stableKey, payloadMobility);
				}
				continue;
			}

			if (bRequiredCustomDepth)
			{
				auto customData = MakeCustomInstanceData(data, *mesh);
				customPacket.Add(
					std::move(batch),
					mesh,
					customData,
					stableKey,
					payloadMobility);
			}
			else
			{
				packet.Add(
					std::move(batch),
					mesh,
					data,
					stableKey,
					payloadMobility);
			}
		}

		for (size_t groupIndex = 0u;
			groupIndex < source->m_instancedGroups.Num(); ++groupIndex)
		{
			const auto& group = source->m_instancedGroups[groupIndex];
			for (size_t meshIndex = 0u; meshIndex < group.m_meshes.Num(); ++meshIndex)
			{
				if (meshIndex >= group.m_materials.Num())
				{
					break;
				}

				const auto& sourceMaterial = group.m_materials[meshIndex];
				if (!sourceMaterial ||
					sourceMaterial->GetRenderState().GetTag() != queueTagHash)
				{
					continue;
				}

				const auto& sourceMesh = group.m_meshes[meshIndex];
				if (!sourceMesh)
				{
					continue;
				}

				const bool bSkinned =
					proxy.GetSkeletonOffset() != (std::numeric_limits<uint32_t>::max)() &&
					sourceMesh->m_vertexDescription->HasAttribute(RHI::RHIVertexDescription::DefaultBoneIdsBinding) &&
					sourceMesh->m_vertexDescription->HasAttribute(RHI::RHIVertexDescription::DefaultBoneWeightsBinding);
				auto depthMaterial = GetOrAddDepthMaterial(
					sourceMaterial, sourceMesh->m_vertexDescription, bSkinned);
				const bool bRequiredCustomDepth =
					sourceMaterial->GetRenderState().IsRequiredCustomDepthShader();
				const bool bIsDepthMaterialReady = depthMaterial &&
					preparedMaterials.Get(depthMaterial).m_bHasGraphicsShaders &&
					depthMaterial->GetRenderState().IsEnabledZWrite();
				if (!bIsDepthMaterialReady)
				{
					continue;
				}

				RHIBatch batchTemplate = preparedMaterials.MakeBatch(depthMaterial, sourceMesh);
				const auto materialData = GetDepthMaterialData(sourceMaterial->GetRenderState(),
					bRequiredCustomDepth ? preparedMaterials.Get(depthMaterial).m_materialInstance : 0u,
					meshIndex < group.m_baseColorSamplers.Num() ? group.m_baseColorSamplers[meshIndex] : 0u,
					meshIndex < group.m_baseColorFactors.Num() ? group.m_baseColorFactors[meshIndex].a : 1.0f,
					meshIndex < group.m_alphaCutoffs.Num() ? group.m_alphaCutoffs[meshIndex] : 0.5f);

				if (bRequiredCustomDepth || bMaskedQueue)
				{
					uint32_t supportedMeshesPerBatch = (std::numeric_limits<uint32_t>::max)();
#if defined(__APPLE__)
					bool bCurrentTextureBindings = false;
					const auto& requestedTextures =
						meshIndex < group.m_materialTextureSamplers.Num() ?
						group.m_materialTextureSamplers[meshIndex] :
						Framegraph::Details::GetDefaultRequestedTextures();
					batchTemplate.m_textureBindings = Framegraph::Details::GetTextureBindingSet(
						m_textureBindingCache,
						requestedTextures,
						sceneView.m_frame,
						supportedMeshesPerBatch,
						bCurrentTextureBindings);
#else
					batchTemplate.m_textureBindings = App::GetSubmodule<TextureImporter>()->GetTextureSamplersBindingSet();
#endif
					batchTemplate.m_supportedMeshesPerBatch = supportedMeshesPerBatch;
					if (!batchTemplate.m_textureBindings)
					{
						continue;
					}
				}

				for (size_t instanceIndex = 0u;
					instanceIndex < group.m_instanceTransforms.Num(); ++instanceIndex)
				{
					if (!proxy.IsInstancedMeshWithinDistance(
						group,
						instanceIndex,
						meshIndex,
						glm::vec3(sceneView.m_cameraTransform.m_position),
						source->m_lodPolicy.m_maxCameraDistance))
					{
						continue;
					}
					const auto& mesh = sceneView.ResolveInstancedMesh(
						proxy, groupIndex, instanceIndex, meshIndex);
					if (!mesh)
					{
						continue;
					}
					const uint64_t stableKey = BuildPackedDrawStableKey(
						proxy.m_handle,
						source->m_staticMeshEcs,
						static_cast<uint32_t>(groupIndex + 1u),
						static_cast<uint32_t>(meshIndex),
						static_cast<uint32_t>(instanceIndex));
					RHIBatch batch = batchTemplate;
					batch.m_mesh = mesh;
					if (bArenaView)
					{
						const uint64_t rangeKey = BuildPackedDrawRangeKey(
							proxy.m_handle, source->m_staticMeshEcs, proxy.m_resource);
						if (bRequiredCustomDepth)
						{
							customPacket.AddArenaView(
								std::move(batch), mesh, rangeKey, stableKey, payloadMobility);
						}
						else
						{
							packet.AddArenaView(
								std::move(batch), mesh, rangeKey, stableKey, payloadMobility);
						}
						continue;
					}

					auto data = MakeInstanceData(
						proxy.ResolveInstancedMeshWorldMatrix(group, instanceIndex, meshIndex), *mesh,
						bSkinned ? proxy.GetSkeletonOffset() : (std::numeric_limits<uint32_t>::max)(), materialData);

					if (bRequiredCustomDepth)
					{
						auto customData = MakeCustomInstanceData(data, *mesh);
						customPacket.Add(
							std::move(batch),
							mesh,
							customData,
							stableKey,
							payloadMobility);
					}
					else
					{
						packet.Add(
							std::move(batch),
							mesh,
							data,
							stableKey,
							payloadMobility);
					}
				}
			}
		}
	}
}

void DepthPrepassNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	m_drawCallStats = {};
	if (!sceneView.m_submissionContext)
	{
		return;
	}

	auto resources = sceneView.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(
		this,
		sceneView.m_cameraIndex,
		0u);
	if (resources->m_packet.GetNumInstances() == 0u && resources->m_customPacket.GetNumInstances() == 0u)
	{
		return;
	}

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	auto depthAttachment = GetRHIResource("depthStencil").StaticCast<RHI::RHITexture>();
	if (!depthAttachment)
	{
		depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");
	}
	if (!depthAttachment)
	{
		return;
	}

	std::string gpuCullingSetting;
	TryGetString("GPUCulling", gpuCullingSetting);
	const uint32_t compactCount = resources->m_packet.GetNumStorageInstances();
	const uint32_t compactIndexCount = resources->m_packet.GetNumDrawInstances();
	const bool bGpuCullingRequested =
		compactCount > 0u && gpuCullingSetting == "true";
	const size_t compactIndexCapacity =
		static_cast<size_t>(compactIndexCount) * (bGpuCullingRequested ? 2u : 1u);
	if (compactCount > 0u && compactIndexCount > 0u &&
		(!resources->m_perInstanceData ||
			resources->m_sizePerInstanceData < sizeof(PerInstanceData) * compactCount ||
			resources->m_sizeInstanceIndices < sizeof(uint32_t) * compactIndexCapacity))
	{
		resources->m_perInstanceData = driver->CreateShaderBindings();
		driver->AddSsboToShaderBindings(resources->m_perInstanceData, "data", sizeof(PerInstanceData), compactCount, 0u);
		driver->AddSsboToShaderBindings(resources->m_perInstanceData, "indices", sizeof(uint32_t), compactIndexCapacity, 1u);
		resources->m_sizePerInstanceData = sizeof(PerInstanceData) * compactCount;
		resources->m_sizeInstanceIndices = sizeof(uint32_t) * compactIndexCapacity;
	}

	const uint32_t customCount = resources->m_customPacket.GetNumStorageInstances();
	const uint32_t customIndexCount = resources->m_customPacket.GetNumDrawInstances();
	if (customCount > 0u && customIndexCount > 0u &&
		(!resources->m_customPerInstanceData ||
			resources->m_sizeCustomPerInstanceData < sizeof(CustomPerInstanceData) * customCount ||
			resources->m_sizeCustomInstanceIndices < sizeof(uint32_t) * customIndexCount))
	{
		resources->m_customPerInstanceData = driver->CreateShaderBindings();
		driver->AddSsboToShaderBindings(resources->m_customPerInstanceData, "data", sizeof(CustomPerInstanceData), customCount, 0u);
		driver->AddSsboToShaderBindings(resources->m_customPerInstanceData, "indices", sizeof(uint32_t), customIndexCount, 1u);
		resources->m_sizeCustomPerInstanceData = sizeof(CustomPerInstanceData) * customCount;
		resources->m_sizeCustomInstanceIndices = sizeof(uint32_t) * customIndexCount;
	}

	if (resources->m_indirectBuffers.IsEmpty())
	{
		resources->m_indirectBuffers.Resize(1u);
	}
	if (resources->m_cullingIndirectBufferBinding.IsEmpty())
	{
		resources->m_cullingIndirectBufferBinding.Resize(1u);
		resources->m_cullingIndirectBufferBinding[0] = driver->CreateShaderBindings();
	}

	bool bGpuCullingEnabled = bGpuCullingRequested;
	if (bGpuCullingEnabled && !m_pComputeMeshCullingShader)
	{
		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeMeshCulling.shader"))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader(
				shaderInfo->GetFileId(),
				m_pComputeMeshCullingShader,
				{ "DEPTH_INSTANCE_LAYOUT" });
		}
	}

	RHIShaderPtr cullingShader;
	if (bGpuCullingEnabled && m_pComputeMeshCullingShader && m_pComputeMeshCullingShader->IsReady())
	{
		auto depthHighZ = GetResolvedAttachment("depthHighZ").StaticCast<RHI::RHITexture>();
		if (depthHighZ &&
			(!resources->m_computeMeshCullingBindings ||
				resources->m_cullingDepthHighZ != depthHighZ))
		{
			resources->m_computeMeshCullingBindings = driver->CreateShaderBindings();
			driver->AddSamplerToShaderBindings(
				resources->m_computeMeshCullingBindings,
				"depthHighZ",
				depthHighZ,
				0u);
			resources->m_cullingDepthHighZ = depthHighZ;
		}
		if (!depthHighZ)
		{
			bGpuCullingEnabled = false;
		}
#ifdef _DEBUG
		cullingShader = bGpuCullingEnabled ?
			m_pComputeMeshCullingShader->GetDebugComputeShaderRHI() : RHIShaderPtr{};
#else
		cullingShader = bGpuCullingEnabled ?
			m_pComputeMeshCullingShader->GetComputeShaderRHI() : RHIShaderPtr{};
#endif
	}

	const auto viewport = glm::ivec4(
		0,
		depthAttachment->GetExtent().y,
		depthAttachment->GetExtent().x,
		-depthAttachment->GetExtent().y);
	const auto scissors = glm::uvec4(0, 0, depthAttachment->GetExtent().x, depthAttachment->GetExtent().y);
	const auto collectCompactBindings = [&](
		const RHIBatch& batch,
		TVector<RHIShaderBindingSetPtr>& sets)
		{
			sets.Add(sceneView.m_frameBindings);
			sets.Add(resources->m_perInstanceData);
			if (batch.m_textureBindings)
			{
				sets.Add(batch.m_textureBindings);
			}
			const bool bSkinned =
				batch.m_mesh->m_vertexDescription->HasAttribute(RHIVertexDescription::DefaultBoneIdsBinding) &&
				batch.m_mesh->m_vertexDescription->HasAttribute(RHIVertexDescription::DefaultBoneWeightsBinding);
			if (bSkinned && sceneView.m_boneMatrices)
			{
				sets.Add(sceneView.m_boneMatrices);
			}
		};
	const auto collectCustomBindings = [&](
		const RHIBatch& batch,
		TVector<RHIShaderBindingSetPtr>& sets)
		{
			sets.Add(sceneView.m_frameBindings);
			sets.Add(sceneView.m_rhiLightsData);
			sets.Add(resources->m_customPerInstanceData);
			sets.Add(batch.GetMaterialBindings());
			sets.Add(batch.m_textureBindings);
			const bool bSkinned =
				batch.m_mesh->m_vertexDescription->HasAttribute(RHIVertexDescription::DefaultBoneIdsBinding) &&
				batch.m_mesh->m_vertexDescription->HasAttribute(RHIVertexDescription::DefaultBoneWeightsBinding);
			if (bSkinned && sceneView.m_boneMatrices)
			{
				sets.Add(sceneView.m_boneMatrices);
			}
		};

	std::string clearDepth;
	TryGetString("ClearDepth", clearDepth);
	commands->BeginDebugRegion(
		commandList,
		std::string(GetName()) + " QueueTag:" + GetString("Tag") + " Packed",
		DebugContext::Color_CmdGraphics);
	const auto depthLayout = RHI::IsDepthStencilFormat(depthAttachment->GetFormat()) ?
		EImageLayout::DepthStencilAttachmentOptimal : EImageLayout::DepthAttachmentOptimal;
	commands->ImageMemoryBarrier(commandList, depthAttachment, depthLayout);
	static const TVector<RHI::RHITexturePtr> NoColorAttachments;
	commands->BeginRenderPass(
		commandList,
		NoColorAttachments,
		depthAttachment,
		glm::vec4(0, 0, depthAttachment->GetExtent().x, depthAttachment->GetExtent().y),
		glm::ivec2(0, 0),
		clearDepth == "true",
		glm::vec4(0.0f),
		0.0f,
		true,
		true);

	if (compactCount > 0u)
	{
		auto& cullingBindings = resources->m_cullingDispatchBindings;
		cullingBindings.Clear(false);
		if (cullingShader)
		{
			cullingBindings = {
				resources->m_computeMeshCullingBindings,
				resources->m_perInstanceData,
				resources->m_cullingIndirectBufferBinding[0],
				sceneView.m_frameBindings };
		}
		m_drawCallStats += RHIRecordPackedDrawPacket(
			resources->m_packet,
			commandList,
			transferCommandList,
			collectCompactBindings,
			resources->m_perInstanceData,
			resources->m_indirectBuffers[0],
			viewport,
			scissors,
			glm::vec2(0.0f, 1.0f),
			cullingShader,
			&resources->m_cullingIndirectBufferBinding[0],
			cullingBindings);
	}
	if (customCount > 0u)
	{
		m_drawCallStats += RHIRecordPackedDrawPacket(
			resources->m_customPacket,
			commandList,
			transferCommandList,
			collectCustomBindings,
			resources->m_customPerInstanceData,
			resources->m_customIndirectBuffer,
			viewport,
			scissors);
	}

	commands->EndRenderPass(commandList);
	commands->EndDebugRegion(commandList);
}

void DepthPrepassNode::Clear()
{
	m_textureBindingCache.Clear();
	m_pagedArenaCache.Clear();
	m_customPagedArenaCache.Clear();
}
