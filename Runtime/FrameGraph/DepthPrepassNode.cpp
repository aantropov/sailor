#include "DepthPrepassNode.h"
#include "Core/StringHash.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "RHI/Types.h"
#include "RHI/Batch.hpp"
#include "RHI/VertexDescription.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/AssetRegistry.h"

#include <limits>

using namespace Sailor;
using namespace Sailor::RHI;

#ifndef _SAILOR_IMPORT_
const char* DepthPrepassNode::m_name = "DepthPrepass";
#endif

RHI::RHIMaterialPtr DepthPrepassNode::GetOrAddDepthMaterial(
	RHI::RHIVertexDescriptionPtr vertexDescription,
	bool bSkinned,
	bool bMasked,
	RHI::ECullMode cullMode)
{
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
	const bool bMaskedQueue = QueueTagHash == "Masked"_h.GetHash();
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
			const uint64_t materialSubmissionId =
				sceneViewSnapshot.m_submissionContext->GetSubmissionId();

			auto submissionResources = sceneViewSnapshot.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(
				this,
				sceneViewSnapshot.m_cameraIndex,
				0u);
			auto& m_packet = submissionResources->m_packet;
			auto& m_customPacket = submissionResources->m_customPacket;

			syncSharedResources.Lock();
			auto& requestedPacketTextures =
				submissionResources->m_requestedPacketTextures;
			for (auto& requestedTextures : requestedPacketTextures)
			{
				requestedTextures.Reset();
			}

			SAILOR_PROFILE_SCOPE("Filter sceneView by tag");

			constexpr size_t PayloadRevisionSeed = Fnv1aOffsetBasis;
			std::array<size_t, RHI::TPackedDrawPacket<PerInstanceData>::NumMobilitySegments>
				payloadRevisions{};
			std::array<uint32_t, RHI::TPackedDrawPacket<PerInstanceData>::NumMobilitySegments>
				numRelevantProxies{};
			std::array<bool, RHI::TPackedDrawPacket<PerInstanceData>::NumMobilitySegments>
				bUsesPacketTextures{};
			for (size_t index = 0u; index < payloadRevisions.size(); ++index)
			{
				payloadRevisions[index] = PayloadRevisionSeed;
				HashCombine(payloadRevisions[index], index);
				bUsesPacketTextures[index] = bMaskedQueue;
			}
			const size_t staticPayloadIndex =
				RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(
					EMobilityType::Static);
			const size_t stationaryPayloadIndex =
				RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(
					EMobilityType::Stationary);
			const bool bUsesPagedArenas = bVirtualizeInstancePayloads;
			auto usesPagedArena = [&](size_t payloadIndex)
				{
					return bUsesPagedArenas &&
						(payloadIndex == staticPayloadIndex ||
							payloadIndex == stationaryPayloadIndex);
				};
			if (bUsesPagedArenas)
			{
				for (const EMobilityType mobility :
					{ EMobilityType::Static, EMobilityType::Stationary })
				{
					const size_t payloadIndex =
						RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(mobility);
					payloadRevisions[payloadIndex] = PayloadRevisionSeed;
					HashCombine(
						payloadRevisions[payloadIndex],
						payloadIndex,
						QueueTagHash,
						bMaskedQueue,
						sceneViewSnapshot.m_submissionContext->GetMaterialRevision(),
						sceneViewSnapshot.GetMobilityRevision(mobility));
				}
			}
			for (const auto& proxy : sceneViewSnapshot.m_proxies)
			{
				const auto* source = proxy.GetSource();
				if (!source || !proxy.m_resource)
				{
					continue;
				}
				const EMobilityType payloadMobility = proxy.GetMobility();
				const size_t payloadIndex =
					RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(payloadMobility);
				bool bContributesToQueue = false;
				for (size_t materialIndex = 0u;
					materialIndex < source->GetMaterials().Num(); ++materialIndex)
				{
					const auto& material = source->GetMaterials()[materialIndex];
					const bool bRelevant = material &&
						material->GetRenderState().GetTag() == QueueTagHash;
					bContributesToQueue |= bRelevant;
					const bool bCustom = bRelevant &&
						material->GetRenderState().IsRequiredCustomDepthShader();
					bUsesPacketTextures[payloadIndex] =
						bUsesPacketTextures[payloadIndex] || bCustom;
#if defined(__APPLE__)
					if ((bMaskedQueue || bCustom) &&
						materialIndex < source->m_materialTextureSamplers.Num())
					{
						for (uint32_t texture : source->m_materialTextureSamplers[materialIndex])
						{
							requestedPacketTextures[payloadIndex].Insert(texture);
						}
					}
#endif
				}
				for (const auto& group : source->m_instancedGroups)
				{
					for (size_t materialIndex = 0u;
						materialIndex < group.m_materials.Num(); ++materialIndex)
					{
						const auto& material = group.m_materials[materialIndex];
						const bool bRelevant = material &&
							material->GetRenderState().GetTag() == QueueTagHash;
						bContributesToQueue |= bRelevant;
						const bool bCustom = bRelevant &&
							material->GetRenderState().IsRequiredCustomDepthShader();
						bUsesPacketTextures[payloadIndex] =
							bUsesPacketTextures[payloadIndex] || bCustom;
#if defined(__APPLE__)
						if ((bMaskedQueue || bCustom) &&
							materialIndex < group.m_materialTextureSamplers.Num())
						{
							for (uint32_t texture : group.m_materialTextureSamplers[materialIndex])
							{
								requestedPacketTextures[payloadIndex].Insert(texture);
							}
						}
#endif
					}
				}
				if (bContributesToQueue)
				{
					++numRelevantProxies[payloadIndex];
					if (!usesPagedArena(payloadIndex))
					{
						HashCombine(
							payloadRevisions[payloadIndex],
							proxy.m_handle.m_slot,
							proxy.m_handle.m_generation,
							proxy.m_resource->m_depthRevision,
							source->m_staticMeshEcs,
							std::hash<glm::mat4>{}(proxy.GetWorldMatrix()),
							proxy.GetSkeletonOffset());
						for (size_t meshIndex = 0u; meshIndex < source->m_meshes.Num(); ++meshIndex)
						{
							if (meshIndex < source->GetMaterials().Num() &&
								source->GetMaterials()[meshIndex] &&
								source->GetMaterials()[meshIndex]->GetRenderState().GetTag() == QueueTagHash)
							{
								HashCombine(payloadRevisions[payloadIndex], proxy.ResolveMesh(meshIndex));
							}
						}
						for (const auto& group : source->m_instancedGroups)
						{
							for (size_t meshIndex = 0u; meshIndex < group.m_meshes.Num(); ++meshIndex)
							{
								if (meshIndex < group.m_materials.Num() &&
									group.m_materials[meshIndex] &&
									group.m_materials[meshIndex]->GetRenderState().GetTag() == QueueTagHash)
								{
									HashCombine(payloadRevisions[payloadIndex], proxy.ResolveMesh(group.m_meshes[meshIndex]));
							}
						}
						}
					}
				}
			}
			for (size_t index = 0u; index < payloadRevisions.size(); ++index)
			{
				const uint64_t textureDescriptorRevision = bUsesPacketTextures[index] ?
					Framegraph::Details::CalculateTextureDependencyRevision(
						requestedPacketTextures[index].GetIndices()) : 0ull;
				if (!usesPagedArena(index))
				{
					HashCombine(
						payloadRevisions[index],
						numRelevantProxies[index],
						QueueTagHash,
						bMaskedQueue,
						textureDescriptorRevision);
				}
			}
			m_packet.Reset();
			m_customPacket.Reset();
			std::array<bool, RHI::TPackedDrawPacket<PerInstanceData>::NumMobilitySegments>
				bBuildPacketPayload{ true, true, true };
			std::array<bool, RHI::TPackedDrawPacket<CustomPerInstanceData>::NumMobilitySegments>
				bBuildCustomPayload{ true, true, true };
			std::array<bool, RHI::TPackedDrawPacket<PerInstanceData>::NumMobilitySegments>
				bPacketPayloadComplete{ true, true, true };
			std::array<bool, RHI::TPackedDrawPacket<CustomPerInstanceData>::NumMobilitySegments>
				bCustomPayloadComplete{ true, true, true };
			auto buildPayloadCacheSlot = [&](EMobilityType mobility)
				{
					size_t cacheSlot = PayloadRevisionSeed;
					const size_t index =
						RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(mobility);
					const uint32_t cameraIndex = usesPagedArena(index) ?
						0u : sceneViewSnapshot.m_cameraIndex;
					HashCombine(cacheSlot, cameraIndex, index);
					return cacheSlot;
				};
			if (bVirtualizeInstancePayloads)
			{
				for (const EMobilityType mobility :
					{ EMobilityType::Static, EMobilityType::Stationary })
				{
					const size_t index =
						RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(mobility);
					const size_t cacheSlot = buildPayloadCacheSlot(mobility);
					auto packetPayload = usesPagedArena(index) ?
						m_pagedArenaCache.Find(
							cacheSlot,
							payloadRevisions[index],
							sceneViewSnapshot.m_frame) :
						m_packetPayloadCache.Find(
							cacheSlot,
							payloadRevisions[index],
							sceneViewSnapshot.m_frame);
					if (packetPayload)
					{
						if (usesPagedArena(index))
						{
							m_packet.UseSharedArenaPayload(mobility, std::move(packetPayload));
						}
						else
						{
							m_packet.UseSharedPayload(mobility, std::move(packetPayload));
						}
						bBuildPacketPayload[index] = false;
					}
					auto customPayload = usesPagedArena(index) ?
						m_customPagedArenaCache.Find(
							cacheSlot,
							payloadRevisions[index],
							sceneViewSnapshot.m_frame) :
						m_customPacketPayloadCache.Find(
							cacheSlot,
							payloadRevisions[index],
							sceneViewSnapshot.m_frame);
					if (customPayload)
					{
						if (usesPagedArena(index))
						{
							m_customPacket.UseSharedArenaPayload(mobility, std::move(customPayload));
						}
						else
						{
							m_customPacket.UseSharedPayload(mobility, std::move(customPayload));
						}
						bBuildCustomPayload[index] = false;
					}
				}
			}

			if (bUsesPagedArenas)
			{
				for (const EMobilityType mobility : {EMobilityType::Static, EMobilityType::Stationary})
				{
					const size_t arenaPayloadIndex = RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(mobility);
					if (!bBuildPacketPayload[arenaPayloadIndex] && !bBuildCustomPayload[arenaPayloadIndex])
					{
						continue;
					}
					const size_t arenaCacheSlot = buildPayloadCacheSlot(mobility);
					if (bBuildPacketPayload[arenaPayloadIndex])
					{
						m_pagedArenaCache.BeginUpdate(
							arenaCacheSlot, payloadRevisions[arenaPayloadIndex], sceneViewSnapshot.m_frame);
					}
					if (bBuildCustomPayload[arenaPayloadIndex])
					{
						m_customPagedArenaCache.BeginUpdate(
							arenaCacheSlot, payloadRevisions[arenaPayloadIndex], sceneViewSnapshot.m_frame);
					}
					auto& rangeInstances = submissionResources->m_arenaRangeInstances;
					auto& rangeStableKeys = submissionResources->m_arenaRangeStableKeys;
					auto& rangeMaterialVersionRuns = submissionResources->m_arenaRangeMaterialVersionRuns;
					auto& customRangeInstances = submissionResources->m_customArenaRangeInstances;
					auto& customRangeStableKeys = submissionResources->m_customArenaRangeStableKeys;
					auto& customRangeMaterialVersionRuns = submissionResources->m_customArenaRangeMaterialVersionRuns;
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
						if (!sourceMaterial || sourceMaterial->GetRenderState().GetTag() != QueueTagHash)
						{
							return;
						}
						const bool bRequiredCustomDepth =
							!bMaskedQueue && sourceMaterial->GetRenderState().IsRequiredCustomDepthShader();
						if ((bRequiredCustomDepth && !bBuildCustomRange) ||
							(!bRequiredCustomDepth && !bBuildPacketRange))
						{
							return;
						}
						if (!mesh)
						{
							if (bRequiredCustomDepth)
							{
								bCustomPayloadComplete[arenaPayloadIndex] = false;
							}
							else
							{
								bPacketPayloadComplete[arenaPayloadIndex] = false;
							}
							return;
						}

						const bool bSkinned = proxy.GetSkeletonOffset() != (std::numeric_limits<uint32_t>::max)() &&
							mesh->m_vertexDescription->HasAttribute(RHIVertexDescription::DefaultBoneIdsBinding) &&
							mesh->m_vertexDescription->HasAttribute(RHIVertexDescription::DefaultBoneWeightsBinding);
						auto depthMaterial = GetOrAddDepthMaterial(mesh->m_vertexDescription,
							bSkinned,
							bMaskedQueue,
							sourceMaterial->GetRenderState().GetCullMode());
						if (bRequiredCustomDepth)
						{
							depthMaterial = sourceMaterial;
						}
						const bool bReady = depthMaterial && depthMaterial->GetVertexShader() &&
							depthMaterial->GetFragmentShader() && depthMaterial->GetRenderState().IsEnabledZWrite();
						if (!bReady)
						{
							if (bRequiredCustomDepth)
							{
								bCustomPayloadComplete[arenaPayloadIndex] = false;
							}
							else
							{
								bPacketPayloadComplete[arenaPayloadIndex] = false;
							}
							return;
						}

						RHIBatch batch(depthMaterial, mesh, materialSubmissionId);
						PerInstanceData data;
						data.model = model;
						data.sphereBounds = mesh->m_bounds.ToSphere().GetVec4();
						data.skeletonOffset =
							bSkinned ? proxy.GetSkeletonOffset() : (std::numeric_limits<uint32_t>::max)();
						if (bRequiredCustomDepth)
						{
							const auto bindings = batch.GetMaterialBindings();
							if (bindings && bindings->GetShaderBindings().ContainsKey("material"))
							{
								const auto binding = bindings->GetShaderBindings()["material"];
								data.materialInstance = binding ? binding->GetStorageInstanceIndex() : 0u;
							}
						}
						else if (bMaskedQueue)
						{
							data.materialInstance = baseColorSampler;
							const float effectiveAlphaCutoff =
								baseColorFactor.a > 0.000001f ? alphaCutoff / baseColorFactor.a : 2.0f;
							data.padding = glm::floatBitsToUint(effectiveAlphaCutoff);
						}

						if (bRequiredCustomDepth)
						{
							CustomPerInstanceData customData;
							customData.model = data.model;
							customData.sphereBounds = data.sphereBounds;
							customData.materialInstance = data.materialInstance;
							customData.skeletonOffset = data.skeletonOffset;
							customData.padding = data.padding;
							customData.bakedVolumeScale = vec4(mesh->m_bakedVolumeScale, 1.0f);
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

					sceneViewSnapshot.ForEachSceneProxy(mobility,
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
								QueueTagHash,
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
									if (bMaskedQueue || !material ||
										material->GetRenderState().GetTag() != QueueTagHash ||
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
							bBuildPacketRange = bBuildPacketPayload[arenaPayloadIndex] &&
								!m_pagedArenaCache.TryReuseRange(rangeKey, rangeRevision);
							bBuildCustomRange = bBuildCustomPayload[arenaPayloadIndex] &&
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
								bPacketPayloadComplete[arenaPayloadIndex] = false;
							}
							if (bBuildCustomRange &&
								!m_customPagedArenaCache.ReplaceRange(rangeKey,
									rangeRevision,
									customRangeInstances,
									customRangeStableKeys,
									&customRangeMaterialVersionRuns))
							{
								bCustomPayloadComplete[arenaPayloadIndex] = false;
							}
						});

					if (bBuildPacketPayload[arenaPayloadIndex])
					{
						auto packetPayload = m_pagedArenaCache.EndUpdate(bPacketPayloadComplete[arenaPayloadIndex]);
						m_packet.UseSharedArenaPayload(mobility, std::move(packetPayload));
					}
					if (bBuildCustomPayload[arenaPayloadIndex])
					{
						auto customPayload =
							m_customPagedArenaCache.EndUpdate(bCustomPayloadComplete[arenaPayloadIndex]);
						m_customPacket.UseSharedArenaPayload(mobility, std::move(customPayload));
					}
					rangeInstances.Clear(false);
					rangeStableKeys.Clear(false);
					rangeMaterialVersionRuns.Clear(false);
					customRangeInstances.Clear(false);
					customRangeStableKeys.Clear(false);
					customRangeMaterialVersionRuns.Clear(false);
					bBuildPacketPayload[arenaPayloadIndex] = false;
					bBuildCustomPayload[arenaPayloadIndex] = false;
				}
			}
			Framegraph::Details::EvictTextureBindingCache(m_textureBindingCache, sceneViewSnapshot.m_frame);
			m_packetPayloadCache.Evict(sceneViewSnapshot.m_frame);
			m_customPacketPayloadCache.Evict(sceneViewSnapshot.m_frame);
			m_pagedArenaCache.Evict(sceneViewSnapshot.m_frame);
			m_customPagedArenaCache.Evict(sceneViewSnapshot.m_frame);
			for (const auto& proxy : sceneViewSnapshot.m_proxies)
			{
				const auto* source = proxy.GetSource();
				if (!source)
				{
					continue;
				}
				const EMobilityType payloadMobility = proxy.GetMobility();
				const size_t payloadIndex =
					RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(payloadMobility);
				const bool bArenaView = usesPagedArena(payloadIndex);
				if (!bBuildPacketPayload[payloadIndex] &&
					!bBuildCustomPayload[payloadIndex] && !bArenaView)
				{
					continue;
				}
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
					if (sourceMaterial->GetRenderState().GetTag() != QueueTagHash)
					{
						continue;
					}

					const auto mesh = proxy.ResolveMesh(i);
					if (!mesh)
					{
						const bool bExpectedCustomDepth = !bMaskedQueue &&
							sourceMaterial->GetRenderState().IsRequiredCustomDepthShader();
						if (bExpectedCustomDepth)
						{
							bCustomPayloadComplete[payloadIndex] = false;
						}
						else
						{
							bPacketPayloadComplete[payloadIndex] = false;
						}
						continue;
					}

					const bool bSkinned =
						proxy.GetSkeletonOffset() != (std::numeric_limits<uint32_t>::max)() &&
						mesh->m_vertexDescription->HasAttribute(RHI::RHIVertexDescription::DefaultBoneIdsBinding) &&
						mesh->m_vertexDescription->HasAttribute(RHI::RHIVertexDescription::DefaultBoneWeightsBinding);
					auto depthMaterial = GetOrAddDepthMaterial(
						mesh->m_vertexDescription,
						bSkinned,
						bMaskedQueue,
						sourceMaterial->GetRenderState().GetCullMode());

					const bool bRequiredCustomDepth = !bMaskedQueue &&
						sourceMaterial->GetRenderState().IsRequiredCustomDepthShader();
					if (!bArenaView &&
						((bRequiredCustomDepth && !bBuildCustomPayload[payloadIndex]) ||
						(!bRequiredCustomDepth && !bBuildPacketPayload[payloadIndex])))
					{
						continue;
					}
					if (bRequiredCustomDepth)
					{
						depthMaterial = sourceMaterial;
					}

					const bool bIsDepthMaterialReady = depthMaterial &&
						depthMaterial->GetVertexShader() &&
						depthMaterial->GetFragmentShader() &&
						depthMaterial->GetRenderState().IsEnabledZWrite();

					if (!bIsDepthMaterialReady)
					{
						if (bRequiredCustomDepth)
						{
							bCustomPayloadComplete[payloadIndex] = false;
						}
						else
						{
							bPacketPayloadComplete[payloadIndex] = false;
						}
						continue;
					}
					RHIBatch batch(depthMaterial, mesh, materialSubmissionId);

					const glm::mat4 meshWorldMatrix = proxy.ResolveMeshWorldMatrix(i);
					DepthPrepassNode::PerInstanceData data;
					data.model = meshWorldMatrix;
					data.skeletonOffset = bSkinned ? proxy.GetSkeletonOffset() : (std::numeric_limits<uint32_t>::max)();
					data.sphereBounds = mesh->m_bounds.ToSphere().GetVec4();

					if (bRequiredCustomDepth)
					{
						RHIShaderBindingPtr shaderBinding;
						const auto materialBindings = batch.GetMaterialBindings();
						if (materialBindings && materialBindings->GetShaderBindings().ContainsKey("material"))
						{
							shaderBinding = materialBindings->GetShaderBindings()["material"];
						}
						data.materialInstance = shaderBinding.IsValid() ? shaderBinding->GetStorageInstanceIndex() : 0;
					}
					else if (bMaskedQueue)
					{
						data.materialInstance = source->m_baseColorSamplers.Num() > i ?
							source->m_baseColorSamplers[i] : 0u;
						const float baseColorAlpha = source->m_baseColorFactors.Num() > i ?
							source->m_baseColorFactors[i].a : 1.0f;
						const float alphaCutoff = source->m_alphaCutoffs.Num() > i ?
							source->m_alphaCutoffs[i] : 0.5f;
						const float effectiveAlphaCutoff = baseColorAlpha > 0.000001f ?
							alphaCutoff / baseColorAlpha : 2.0f;
						data.padding = glm::floatBitsToUint(effectiveAlphaCutoff);
					}
					else
					{
						data.materialInstance = 0;
					}

					if (bRequiredCustomDepth || bMaskedQueue)
					{
						uint32_t supportedMeshesPerBatch = (std::numeric_limits<uint32_t>::max)();
#if defined(__APPLE__)
						const auto& requestedTextures =
							source->m_materialTextureSamplers.Num() > i ?
							source->m_materialTextureSamplers[i] :
							Framegraph::Details::GetDefaultRequestedTextures();
						batch.m_textureBindings = Framegraph::Details::GetTextureBindingSet(
							m_textureBindingCache,
							requestedTextures,
							sceneViewSnapshot.m_frame,
							supportedMeshesPerBatch);
#else
						batch.m_textureBindings = App::GetSubmodule<TextureImporter>()->GetTextureSamplersBindingSet();
#endif
						batch.m_supportedMeshesPerBatch = supportedMeshesPerBatch;
						if (!batch.m_textureBindings)
						{
							if (bRequiredCustomDepth)
							{
								bCustomPayloadComplete[payloadIndex] = false;
							}
							else
							{
								bPacketPayloadComplete[payloadIndex] = false;
							}
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
						const bool bAdded = bRequiredCustomDepth ?
							m_customPacket.AddArenaView(
								std::move(batch),
								mesh,
								BuildPackedDrawRangeKey(
									proxy.m_handle,
									source->m_staticMeshEcs,
									proxy.m_resource),
								stableKey,
								payloadMobility) :
							m_packet.AddArenaView(
								std::move(batch),
								mesh,
								BuildPackedDrawRangeKey(
									proxy.m_handle,
									source->m_staticMeshEcs,
									proxy.m_resource),
								stableKey,
								payloadMobility);
						if (!bAdded)
						{
							if (bRequiredCustomDepth)
							{
								bCustomPayloadComplete[payloadIndex] = false;
							}
							else
							{
								bPacketPayloadComplete[payloadIndex] = false;
							}
						}
						continue;
					}

					if (bRequiredCustomDepth)
					{
						DepthPrepassNode::CustomPerInstanceData customData;
						customData.model = data.model;
						customData.sphereBounds = data.sphereBounds;
						customData.materialInstance = data.materialInstance;
						customData.skeletonOffset = data.skeletonOffset;
						customData.padding = data.padding;
						customData.bakedVolumeScale = vec4(mesh->m_bakedVolumeScale, 1.0f);
						m_customPacket.Add(
							std::move(batch),
							mesh,
							customData,
							stableKey,
							payloadMobility);
					}
					else
					{
						m_packet.Add(
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
							sourceMaterial->GetRenderState().GetTag() != QueueTagHash)
						{
							continue;
						}

						const auto& sourceMesh = group.m_meshes[meshIndex];
						if (!sourceMesh)
						{
							const bool bExpectedCustomDepth = !bMaskedQueue &&
								sourceMaterial->GetRenderState().IsRequiredCustomDepthShader();
							if (bExpectedCustomDepth)
							{
								bCustomPayloadComplete[payloadIndex] = false;
							}
							else
							{
								bPacketPayloadComplete[payloadIndex] = false;
							}
							continue;
						}

						const bool bSkinned =
							proxy.GetSkeletonOffset() != (std::numeric_limits<uint32_t>::max)() &&
							sourceMesh->m_vertexDescription->HasAttribute(RHI::RHIVertexDescription::DefaultBoneIdsBinding) &&
							sourceMesh->m_vertexDescription->HasAttribute(RHI::RHIVertexDescription::DefaultBoneWeightsBinding);
						auto depthMaterial = GetOrAddDepthMaterial(
							sourceMesh->m_vertexDescription,
							bSkinned,
							bMaskedQueue,
							sourceMaterial->GetRenderState().GetCullMode());
						const bool bRequiredCustomDepth = !bMaskedQueue &&
							sourceMaterial->GetRenderState().IsRequiredCustomDepthShader();
						if (!bArenaView &&
							((bRequiredCustomDepth && !bBuildCustomPayload[payloadIndex]) ||
							(!bRequiredCustomDepth && !bBuildPacketPayload[payloadIndex])))
						{
							continue;
						}
						if (bRequiredCustomDepth)
						{
							depthMaterial = sourceMaterial;
						}

						const bool bIsDepthMaterialReady = depthMaterial &&
							depthMaterial->GetVertexShader() &&
							depthMaterial->GetFragmentShader() &&
							depthMaterial->GetRenderState().IsEnabledZWrite();
						if (!bIsDepthMaterialReady)
						{
							if (bRequiredCustomDepth)
							{
								bCustomPayloadComplete[payloadIndex] = false;
							}
							else
							{
								bPacketPayloadComplete[payloadIndex] = false;
							}
							continue;
						}

						RHIBatch batchTemplate(
							depthMaterial,
							sourceMesh,
							materialSubmissionId);
						uint32_t materialInstance = 0u;
						if (bRequiredCustomDepth)
						{
							const auto materialBindings = batchTemplate.GetMaterialBindings();
							if (materialBindings && materialBindings->GetShaderBindings().ContainsKey("material"))
							{
								const auto shaderBinding = materialBindings->GetShaderBindings()["material"];
								materialInstance = shaderBinding.IsValid() ?
									shaderBinding->GetStorageInstanceIndex() : 0u;
							}
						}
						else if (bMaskedQueue && meshIndex < group.m_baseColorSamplers.Num())
						{
							materialInstance = group.m_baseColorSamplers[meshIndex];
						}

						float effectiveAlphaCutoff = 0.0f;
						if (bMaskedQueue)
						{
							const float baseColorAlpha = meshIndex < group.m_baseColorFactors.Num() ?
								group.m_baseColorFactors[meshIndex].a : 1.0f;
							const float alphaCutoff = meshIndex < group.m_alphaCutoffs.Num() ?
								group.m_alphaCutoffs[meshIndex] : 0.5f;
							effectiveAlphaCutoff = baseColorAlpha > 0.000001f ?
								alphaCutoff / baseColorAlpha : 2.0f;
						}

						if (bRequiredCustomDepth || bMaskedQueue)
						{
							uint32_t supportedMeshesPerBatch = (std::numeric_limits<uint32_t>::max)();
#if defined(__APPLE__)
							const auto& requestedTextures =
								meshIndex < group.m_materialTextureSamplers.Num() ?
								group.m_materialTextureSamplers[meshIndex] :
								Framegraph::Details::GetDefaultRequestedTextures();
							batchTemplate.m_textureBindings = Framegraph::Details::GetTextureBindingSet(
								m_textureBindingCache,
								requestedTextures,
								sceneViewSnapshot.m_frame,
								supportedMeshesPerBatch);
#else
							batchTemplate.m_textureBindings = App::GetSubmodule<TextureImporter>()->GetTextureSamplersBindingSet();
#endif
							batchTemplate.m_supportedMeshesPerBatch = supportedMeshesPerBatch;
							if (!batchTemplate.m_textureBindings)
							{
								if (bRequiredCustomDepth)
								{
									bCustomPayloadComplete[payloadIndex] = false;
								}
								else
								{
									bPacketPayloadComplete[payloadIndex] = false;
								}
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
								glm::vec3(sceneViewSnapshot.m_cameraTransform.m_position),
								source->m_lodPolicy.m_maxCameraDistance))
							{
								continue;
							}
							const auto mesh = proxy.ResolveInstancedMesh(
								group,
								instanceIndex,
								meshIndex,
								sceneViewSnapshot.m_camera->GetViewMatrix(),
								sceneViewSnapshot.m_camera->GetProjectionMatrix());
							if (!mesh)
							{
								if (bRequiredCustomDepth)
								{
									bCustomPayloadComplete[payloadIndex] = false;
								}
								else
								{
									bPacketPayloadComplete[payloadIndex] = false;
								}
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
								const bool bAdded = bRequiredCustomDepth ?
								m_customPacket.AddArenaView(
									std::move(batch),
									mesh,
									BuildPackedDrawRangeKey(
										proxy.m_handle,
										source->m_staticMeshEcs,
										proxy.m_resource),
									stableKey,
									payloadMobility) :
								m_packet.AddArenaView(
									std::move(batch),
									mesh,
									BuildPackedDrawRangeKey(
										proxy.m_handle,
										source->m_staticMeshEcs,
										proxy.m_resource),
									stableKey,
										payloadMobility);
								if (!bAdded)
								{
									if (bRequiredCustomDepth)
									{
										bCustomPayloadComplete[payloadIndex] = false;
									}
									else
									{
										bPacketPayloadComplete[payloadIndex] = false;
									}
								}
								continue;
							}

							DepthPrepassNode::PerInstanceData data;
							data.model = proxy.ResolveInstancedMeshWorldMatrix(
								group,
								instanceIndex,
								meshIndex);
							data.skeletonOffset = bSkinned ? proxy.GetSkeletonOffset() :
								(std::numeric_limits<uint32_t>::max)();
							data.sphereBounds = mesh->m_bounds.ToSphere().GetVec4();
							data.materialInstance = materialInstance;
							if (bMaskedQueue)
							{
								data.padding = glm::floatBitsToUint(effectiveAlphaCutoff);
							}

							if (bRequiredCustomDepth)
							{
								DepthPrepassNode::CustomPerInstanceData customData;
								customData.model = data.model;
								customData.sphereBounds = data.sphereBounds;
								customData.materialInstance = data.materialInstance;
								customData.skeletonOffset = data.skeletonOffset;
								customData.padding = data.padding;
								customData.bakedVolumeScale = vec4(mesh->m_bakedVolumeScale, 1.0f);
								m_customPacket.Add(
									std::move(batch),
									mesh,
									customData,
									stableKey,
									payloadMobility);
							}
							else
							{
								m_packet.Add(
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
			m_packet.Finalize(false);
			m_customPacket.Finalize(false);
			for (const EMobilityType mobility :
				{ EMobilityType::Static, EMobilityType::Stationary })
			{
				const size_t index =
					RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(mobility);
				if (usesPagedArena(index))
				{
					continue;
				}
				if (bVirtualizeInstancePayloads &&
					bBuildPacketPayload[index] && bPacketPayloadComplete[index])
				{
					m_packetPayloadCache.Publish(
						buildPayloadCacheSlot(mobility),
						payloadRevisions[index],
						m_packet.SharePayload(mobility),
						sceneViewSnapshot.m_frame);
				}
				if (bVirtualizeInstancePayloads &&
					bBuildCustomPayload[index] && bCustomPayloadComplete[index])
				{
					m_customPacketPayloadCache.Publish(
						buildPayloadCacheSlot(mobility),
						payloadRevisions[index],
						m_customPacket.SharePayload(mobility),
						sceneViewSnapshot.m_frame);
				}
			}
			syncSharedResources.Unlock();
		}, EThreadType::RHI);

	return res;
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
	m_syncSharedResources.Lock();
	if (resources->m_packet.GetNumInstances() == 0u && resources->m_customPacket.GetNumInstances() == 0u)
	{
		m_syncSharedResources.Unlock();
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
		m_syncSharedResources.Unlock();
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
	m_syncSharedResources.Unlock();
}

void DepthPrepassNode::Clear()
{
	m_textureBindingCache.Clear();
	m_packetPayloadCache.Clear();
	m_customPacketPayloadCache.Clear();
	m_pagedArenaCache.Clear();
	m_customPagedArenaCache.Clear();
}
