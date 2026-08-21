#include "RenderSceneNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/RenderTarget.h"
#include "RHI/Texture.h"
#include "RHI/Types.h"
#include "RHI/VertexDescription.h"
#include "RHI/CommandList.h"
#include "RHI/Buffer.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/AssetRegistry.h"
#include "Core/SpinLock.h"

#include <cmath>
#include <limits>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

static_assert(TextureDependencyCollector::MaxTrackedTextures ==
	TextureImporter::MaxTexturesInScene);

#ifndef _SAILOR_IMPORT_
const char* RenderSceneNode::m_name = "RenderScene";
#endif

namespace
{
	RHIShaderBindingSetPtr CloneBindingsWithSampler(
		const RHIShaderBindingSetPtr& source,
		const std::string& samplerName,
		const RHITexturePtr& sampler,
		uint32_t samplerBinding)
	{
		if (!source || !sampler)
		{
			return source;
		}

		auto& driver = Renderer::GetDriver();
		auto result = driver->CreateShaderBindings();
		for (const auto& entry : source->GetShaderBindings())
		{
			const auto& name = entry.m_first;
			const auto& binding = entry.m_second;
			if (!binding || name == samplerName)
			{
				continue;
			}

			const auto& layout = binding->GetLayout();
			if (layout.m_type == EShaderBindingType::CombinedImageSampler)
			{
				driver->AddSamplerToShaderBindings(
					result,
					name,
					binding->GetTextureBindings(),
					layout.m_binding,
					layout.m_bVariableDescriptorCount,
					layout.m_arrayCount);
			}
			else if (layout.m_type == EShaderBindingType::StorageImage)
			{
				driver->AddStorageImageToShaderBindings(
					result,
					name,
					binding->GetTextureBindings(),
					layout.m_binding);
			}
			else
			{
				driver->AddShaderBinding(result, binding, name, layout.m_binding);
			}
		}

		driver->AddSamplerToShaderBindings(
			result,
			samplerName,
			sampler,
			samplerBinding);
		result->RecalculateCompatibility();
		return result;
	}
}

bool Details::CanReuseRenderSceneTextureBindings(
	uint64_t cachedSourceRevision,
	uint64_t currentSourceRevision,
	bool bHasCachedBindings,
	const TVector<uint64_t>* cachedSlotRevisions,
	const TVector<uint64_t>* currentSlotRevisions)
{
	if (!bHasCachedBindings)
	{
		return false;
	}

	if (cachedSourceRevision == currentSourceRevision)
	{
		return true;
	}

	return cachedSlotRevisions &&
		currentSlotRevisions &&
		*cachedSlotRevisions == *currentSlotRevisions;
}

uint64_t Details::CalculateTextureDependencyRevision(
	const TVector<uint32_t>& requestedTextures)
{
	auto* textureImporter = App::GetSubmodule<TextureImporter>();
	if (!textureImporter)
	{
		return 0ull;
	}

#if defined(__APPLE__)
	return textureImporter->CalculateTextureSamplersRevision(requestedTextures);
#else
	const auto bindings = textureImporter->GetTextureSamplersBindingSet();
	return bindings ? bindings->GetDescriptorRevision() : 0ull;
#endif
}

TVector<uint32_t> Details::BuildDenseTextureRemap(
	const TVector<uint32_t>& globalTextureIndices)
{
	uint32_t maxTextureIndex = 0u;
	for (const uint32_t textureIndex : globalTextureIndices)
	{
		maxTextureIndex = (std::max)(maxTextureIndex, textureIndex);
	}

	TVector<uint32_t> globalToLocal;
	globalToLocal.AddDefault(static_cast<size_t>(maxTextureIndex) + 1u);

	uint32_t nextLocalIndex = 1u;
	for (const uint32_t textureIndex : globalTextureIndices)
	{
		if (textureIndex == 0u ||
			textureIndex >= globalToLocal.Num() ||
			globalToLocal[textureIndex] != 0u)
		{
			continue;
		}

		globalToLocal[textureIndex] = nextLocalIndex++;
	}

	return globalToLocal;
}

RHI::ESortingOrder RenderSceneNode::GetSortingOrder() const
{
	std::string sortOrder;

	if (TryGetString("Sorting", sortOrder))
	{
		return magic_enum::enum_cast<RHI::ESortingOrder>(sortOrder).value_or(RHI::ESortingOrder::FrontToBack);
	}

	return RHI::ESortingOrder::FrontToBack;
}

RHIShaderBindingSetPtr Details::GetTextureBindingSet(
	TextureBindingCache& textureBindingCache,
	const TSet<uint32_t>& requestedTextures,
	uint64_t frame,
	uint32_t& outSupportedMeshesPerBatch)
{
	auto textureImporter = App::GetSubmodule<TextureImporter>();
	if (!textureImporter)
	{
		outSupportedMeshesPerBatch = 1;
		return nullptr;
	}

	auto globalTextureSet = textureImporter->GetTextureSamplersBindingSet();

#if defined(__APPLE__)
	TextureBindingCacheKey key(requestedTextures);
	const uint64_t currentSourceDescriptorRevision = globalTextureSet ? globalTextureSet->GetDescriptorRevision() : 0;
	TextureBindingCacheEntry* cachedEntry = nullptr;
	textureBindingCache.Find(key, cachedEntry);

	if (cachedEntry && Details::CanReuseRenderSceneTextureBindings(
		cachedEntry->m_sourceDescriptorRevision,
		currentSourceDescriptorRevision,
		cachedEntry->m_textureBindings.IsValid()))
	{
		cachedEntry->m_lastUsedFrame = frame;
#ifdef _DEBUG
		if (cachedEntry->m_textureBindings)
		{
			const auto shaderBinding = cachedEntry->m_textureBindings->GetOrAddShaderBinding("textureSamplers");
			const uint32_t actualTextureCount = static_cast<uint32_t>(shaderBinding->GetTextureBindings().Num());
			const uint32_t actualLayoutCount = shaderBinding->GetLayout().m_arrayCount;
			check(actualTextureCount == cachedEntry->m_textureSetSize);
			check(actualLayoutCount == cachedEntry->m_textureSetSize);
		}
#endif
		outSupportedMeshesPerBatch = (std::max)(1u, MaxTextureSlotsPerBatch / (std::max)(1u, cachedEntry->m_textureSetSize));
		return cachedEntry->m_textureBindings;
	}

	key.Materialize();
	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	const auto sourceSnapshot = textureImporter->GetTextureSamplersSnapshot(key.m_requestedTextures);
	TVector<uint64_t> currentSlotRevisions;
	currentSlotRevisions.Reserve(sourceSnapshot.m_slots.Num());
	for (const auto& slot : sourceSnapshot.m_slots)
	{
		currentSlotRevisions.Emplace(slot.m_contentRevision);
	}

	if (cachedEntry && Details::CanReuseRenderSceneTextureBindings(
		cachedEntry->m_sourceDescriptorRevision,
		sourceSnapshot.m_descriptorRevision,
		cachedEntry->m_textureBindings.IsValid(),
		&cachedEntry->m_sourceSlotRevisions,
		&currentSlotRevisions))
	{
		cachedEntry->m_sourceDescriptorRevision = sourceSnapshot.m_descriptorRevision;
		cachedEntry->m_lastUsedFrame = frame;
		outSupportedMeshesPerBatch = (std::max)(1u, MaxTextureSlotsPerBatch / (std::max)(1u, cachedEntry->m_textureSetSize));
		return cachedEntry->m_textureBindings;
	}

	RHITexturePtr defaultTexture = driver->GetDefaultTexture();
	TVector<RHITexturePtr> localTextures{ defaultTexture };
	TVector<uint32_t> globalToLocal =
		Details::BuildDenseTextureRemap(key.m_requestedTextures);
	globalToLocal.Resize(TextureImporter::MaxTexturesInScene);

	for (const auto& slot : sourceSnapshot.m_slots)
	{
		if (slot.m_index != 0u)
		{
			localTextures.Add(slot.m_texture.IsValid() ?
				slot.m_texture :
				defaultTexture);
		}
	}
	const uint32_t denseTextureCount =
		static_cast<uint32_t>(localTextures.Num());

	RHIShaderBindingSetPtr localTextureSet = driver->CreateShaderBindings();
	const size_t remapBufferSize =
		globalToLocal.Num() * sizeof(uint32_t);
	RHI::RHIBufferPtr remapBuffer = driver->CreateBuffer(
		remapBufferSize,
		RHI::EBufferUsageBit::StorageBuffer_Bit,
		RHI::EMemoryPropertyBit::HostVisible |
			RHI::EMemoryPropertyBit::HostCoherent);
	if (!remapBuffer || !remapBuffer->GetPointer())
	{
		outSupportedMeshesPerBatch = 1u;
		return cachedEntry ? cachedEntry->m_textureBindings : nullptr;
	}
	memcpy(remapBuffer->GetPointer(), globalToLocal.GetData(), remapBufferSize);

	driver->AddBufferToShaderBindings(
		localTextureSet,
		remapBuffer,
		"textureSamplerRemap",
		0);
	driver->AddSamplerToShaderBindings(
		localTextureSet,
		"textureSamplers",
		localTextures,
		1,
		true,
		denseTextureCount);
	localTextureSet->RecalculateCompatibility();

#if defined(SAILOR_BUILD_WITH_VULKAN)
	if (!localTextureSet->m_vulkan.m_descriptorSet || !localTextureSet->m_vulkan.m_descriptorSet->IsCompiled())
	{
		if (cachedEntry)
		{
			cachedEntry->m_lastUsedFrame = frame;
			outSupportedMeshesPerBatch = (std::max)(1u, MaxTextureSlotsPerBatch / (std::max)(1u, cachedEntry->m_textureSetSize));
			return cachedEntry->m_textureBindings;
		}

		outSupportedMeshesPerBatch = 1u;
		return nullptr;
	}
#endif

#ifdef _DEBUG
	if (const auto localBinding = localTextureSet->GetOrAddShaderBinding("textureSamplers"))
	{
		const uint32_t actualTextureCount = static_cast<uint32_t>(localBinding->GetTextureBindings().Num());
		const uint32_t actualLayoutCount = localBinding->GetLayout().m_arrayCount;
		check(actualTextureCount == denseTextureCount);
		check(actualLayoutCount == denseTextureCount);
	}
#endif

	auto& entry = textureBindingCache[key];
	entry.m_textureBindings = localTextureSet;
	entry.m_textureRemapBuffer = remapBuffer;
	entry.m_textureSetSize = denseTextureCount;
	entry.m_lastUsedFrame = frame;
	entry.m_sourceDescriptorRevision = sourceSnapshot.m_descriptorRevision;
	entry.m_sourceSlotRevisions = std::move(currentSlotRevisions);

	outSupportedMeshesPerBatch = (std::max)(1u, MaxTextureSlotsPerBatch / (std::max)(1u, entry.m_textureSetSize));
	return entry.m_textureBindings;
#else
	outSupportedMeshesPerBatch = (std::numeric_limits<uint32_t>::max)();
	return globalTextureSet;
#endif
}

void Details::EvictTextureBindingCache(
	TextureBindingCache& textureBindingCache,
	uint64_t frame)
{
	TVector<TextureBindingCacheKey> expiredEntries;

	for (const auto& entry : textureBindingCache)
	{
		if (frame > entry.Second()->m_lastUsedFrame && frame - entry.Second()->m_lastUsedFrame > MaxTextureBindingCacheUnusedFrames)
		{
			expiredEntries.Add(entry.First());
		}
	}

	for (const auto& key : expiredEntries)
	{
		textureBindingCache.Remove(key);
	}
}

Tasks::TaskPtr<void, void> RenderSceneNode::Prepare(RHI::RHIFrameGraphPtr frameGraph, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	const std::string QueueTag = GetString("Tag");
	const size_t QueueTagHash = GetHash(QueueTag);
	const bool bBackToFront = GetSortingOrder() == RHI::ESortingOrder::BackToFront;
	std::string virtualizeInstancePayloadsSetting;
	const bool bVirtualizeInstancePayloads =
		!TryGetString("VirtualizeInstancePayloads", virtualizeInstancePayloadsSetting) ||
		virtualizeInstancePayloadsSetting != "false";

	auto res = Tasks::CreateTask("Prepare RenderSceneNode  " + std::to_string(sceneView.m_frame),
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
			auto& m_orderedDrawItems = submissionResources->m_orderedDrawItems;

			syncSharedResources.Lock();
			auto& requestedPacketTextures =
				submissionResources->m_requestedPacketTextures;
			for (auto& requestedTextures : requestedPacketTextures)
			{
				requestedTextures.Reset();
			}

			constexpr size_t PayloadRevisionSeed = 1469598103934665603ull;
			std::array<size_t, RHI::TPackedDrawPacket<PerInstanceData>::NumMobilitySegments>
				payloadRevisions{};
			std::array<uint32_t, RHI::TPackedDrawPacket<PerInstanceData>::NumMobilitySegments>
				numRelevantProxies{};
			for (size_t index = 0u; index < payloadRevisions.size(); ++index)
			{
				payloadRevisions[index] = PayloadRevisionSeed;
				HashCombine(payloadRevisions[index], index);
			}
			const size_t staticPayloadIndex =
				RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(
					EMobilityType::Static);
			const size_t stationaryPayloadIndex =
				RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(
					EMobilityType::Stationary);
			const bool bUsesPagedArenas = bVirtualizeInstancePayloads && !bBackToFront;
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
				bool bContributesToQueue = false;
				for (size_t renderQueueTag : source->m_renderQueueTags)
				{
					bContributesToQueue |= renderQueueTag == QueueTagHash;
				}
				for (const auto& group : source->m_instancedGroups)
				{
					for (size_t renderQueueTag : group.m_renderQueueTags)
					{
						bContributesToQueue |= renderQueueTag == QueueTagHash;
					}
				}
				const EMobilityType payloadMobility = bBackToFront ?
					EMobilityType::Dynamic : proxy.GetMobility();
				const size_t payloadIndex =
					RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(payloadMobility);
#if defined(__APPLE__)
				for (size_t materialIndex = 0u;
					materialIndex < source->m_materialTextureSamplers.Num(); ++materialIndex)
				{
					if (materialIndex < source->m_renderQueueTags.Num() &&
						source->m_renderQueueTags[materialIndex] == QueueTagHash)
					{
						for (uint32_t texture : source->m_materialTextureSamplers[materialIndex])
						{
							requestedPacketTextures[payloadIndex].Insert(texture);
						}
					}
				}
				for (const auto& group : source->m_instancedGroups)
				{
					for (size_t materialIndex = 0u;
						materialIndex < group.m_materialTextureSamplers.Num(); ++materialIndex)
					{
						if (materialIndex < group.m_renderQueueTags.Num() &&
							group.m_renderQueueTags[materialIndex] == QueueTagHash)
						{
							for (uint32_t texture : group.m_materialTextureSamplers[materialIndex])
							{
								requestedPacketTextures[payloadIndex].Insert(texture);
							}
						}
					}
				}
#endif
				if (bContributesToQueue)
				{
					++numRelevantProxies[payloadIndex];
					if (!usesPagedArena(payloadIndex))
					{
						HashCombine(
							payloadRevisions[payloadIndex],
							proxy.m_handle.m_slot,
							proxy.m_handle.m_generation,
							proxy.m_resource->m_mainRevision,
							source->m_staticMeshEcs,
							std::hash<glm::mat4>{}(proxy.GetWorldMatrix()),
							proxy.GetSkeletonOffset(),
							proxy.GetRenderFlags());
						for (size_t meshIndex = 0u; meshIndex < source->m_meshes.Num(); ++meshIndex)
						{
							if (meshIndex < source->m_renderQueueTags.Num() &&
								source->m_renderQueueTags[meshIndex] == QueueTagHash)
							{
								HashCombine(payloadRevisions[payloadIndex], proxy.ResolveMesh(meshIndex));
							}
						}
						for (const auto& group : source->m_instancedGroups)
						{
							for (size_t meshIndex = 0u; meshIndex < group.m_meshes.Num(); ++meshIndex)
							{
								if (meshIndex < group.m_renderQueueTags.Num() &&
									group.m_renderQueueTags[meshIndex] == QueueTagHash)
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
				const uint64_t textureDescriptorRevision =
					Details::CalculateTextureDependencyRevision(
						requestedPacketTextures[index].GetIndices());
				if (!usesPagedArena(index))
				{
					HashCombine(
						payloadRevisions[index],
						numRelevantProxies[index],
						QueueTagHash,
						bBackToFront,
						textureDescriptorRevision);
				}
			}
			if (bBackToFront)
			{
				HashCombine(
					payloadRevisions[RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(
						EMobilityType::Dynamic)],
					std::hash<glm::mat4>{}(sceneViewSnapshot.m_camera->GetViewMatrix()));
			}
			m_packet.Reset();
			m_orderedDrawItems.Clear(false);
			std::array<bool, RHI::TPackedDrawPacket<PerInstanceData>::NumMobilitySegments>
				bBuildPayload{ true, true, true };
			std::array<bool, RHI::TPackedDrawPacket<PerInstanceData>::NumMobilitySegments>
				bPayloadComplete{ true, true, true };
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
			if (bVirtualizeInstancePayloads && !bBackToFront)
			{
				for (const EMobilityType mobility :
					{ EMobilityType::Static, EMobilityType::Stationary })
				{
					const size_t index =
						RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(mobility);
					const size_t cacheSlot = buildPayloadCacheSlot(mobility);
					auto payload = usesPagedArena(index) ?
						m_pagedArenaCache.Find(
							cacheSlot,
							payloadRevisions[index],
							sceneViewSnapshot.m_frame) :
						m_packetPayloadCache.Find(
							cacheSlot,
							payloadRevisions[index],
							sceneViewSnapshot.m_frame);
					if (payload)
					{
						if (usesPagedArena(index))
						{
							m_packet.UseSharedArenaPayload(mobility, std::move(payload));
						}
						else
						{
							m_packet.UseSharedPayload(mobility, std::move(payload));
						}
						bBuildPayload[index] = false;
					}
				}
			}

			if (bUsesPagedArenas)
			{
				for (const EMobilityType mobility : {EMobilityType::Static, EMobilityType::Stationary})
				{
					const size_t arenaPayloadIndex = RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(mobility);
					if (!bBuildPayload[arenaPayloadIndex])
					{
						continue;
					}
					const size_t arenaCacheSlot = buildPayloadCacheSlot(mobility);
					m_pagedArenaCache.BeginUpdate(
						arenaCacheSlot, payloadRevisions[arenaPayloadIndex], sceneViewSnapshot.m_frame);
					auto& rangeInstances = submissionResources->m_arenaRangeInstances;
					auto& rangeStableKeys = submissionResources->m_arenaRangeStableKeys;
					auto& rangeMaterialVersionRuns = submissionResources->m_arenaRangeMaterialVersionRuns;
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
								proxy.m_resource ? proxy.m_resource->m_mainRevision : proxy.GetContentRevision();
							HashCombine(rangeRevision,
								QueueTagHash,
								proxy.GetContentRevision(),
								std::hash<glm::mat4>{}(proxy.GetWorldMatrix()),
								proxy.GetSkeletonOffset());
							if (proxy.m_record)
							{
								HashCombine(
									rangeRevision, proxy.m_record->m_materialRevision, proxy.m_record->m_renderFlags);
							}
							for (const auto& material : source->GetMaterials())
							{
								const auto version = material ?
									material->GetVersionForSubmission(materialSubmissionId) :
									RHIMaterialVersionPtr{};
								HashCombine(rangeRevision, version ? version->GetVersionId() : 0ull);
							}
							for (const auto& group : source->m_instancedGroups)
							{
								for (const auto& material : group.m_materials)
								{
									const auto version = material ?
										material->GetVersionForSubmission(materialSubmissionId) :
										RHIMaterialVersionPtr{};
									HashCombine(rangeRevision, version ? version->GetVersionId() : 0ull);
								}
							}
							if (m_pagedArenaCache.TryReuseRange(rangeKey, rangeRevision))
							{
								return;
							}

							rangeInstances.Clear(false);
							rangeStableKeys.Clear(false);
							rangeMaterialVersionRuns.Clear(false);
							bool bRangeComplete = true;

							for (size_t meshIndex = 0u; meshIndex < source->m_meshes.Num(); ++meshIndex)
							{
								if (meshIndex >= source->GetMaterials().Num())
								{
									break;
								}
								const auto& material = source->GetMaterials()[meshIndex];
								const auto& mesh = source->m_meshes[meshIndex];
								const bool bRelevantMaterial =
									material && material->GetRenderState().GetTag() == QueueTagHash;
								if (!bRelevantMaterial)
								{
									continue;
								}
								if (!mesh || !material->GetVertexShader() || !material->GetFragmentShader())
								{
									bRangeComplete = false;
									continue;
								}

								RHIBatch batch(material, mesh, materialSubmissionId);
								const auto materialBindings = batch.GetMaterialBindings();
								if (!materialBindings || materialBindings->GetShaderBindings().Num() == 0u)
								{
									bRangeComplete = false;
									continue;
								}

								RHIShaderBindingPtr materialBinding;
								if (materialBindings->GetShaderBindings().ContainsKey("material"))
								{
									materialBinding = materialBindings->GetShaderBindings()["material"];
								}
								PerInstanceData data;
								data.model = proxy.ResolveMeshWorldMatrix(meshIndex);
								data.skeletonOffset = proxy.GetSkeletonOffset();
								data.materialInstance =
									materialBinding ? materialBinding->GetStorageInstanceIndex() : 0u;
								data.bIsCulled = 0u;
								data.sphereBounds = mesh->m_bounds.ToSphere().GetVec4();
								data.bakedVolumeScale = vec4(mesh->m_bakedVolumeScale, 1.0f);
								rangeInstances.Add(std::move(data));
								rangeStableKeys.Add(BuildPackedDrawStableKey(
									proxy.m_handle, source->m_staticMeshEcs, 0u, static_cast<uint32_t>(meshIndex), 0u));
								AppendPackedDrawArenaMaterialVersion(rangeMaterialVersionRuns, batch.m_materialVersion);
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
									const auto& material = group.m_materials[meshIndex];
									const auto& mesh = group.m_meshes[meshIndex];
									const bool bRelevantMaterial =
										material && material->GetRenderState().GetTag() == QueueTagHash;
									if (!bRelevantMaterial)
									{
										continue;
									}
									if (!mesh || !material->GetVertexShader() || !material->GetFragmentShader())
									{
										bRangeComplete = false;
										continue;
									}

									RHIBatch batch(material, mesh, materialSubmissionId);
									const auto materialBindings = batch.GetMaterialBindings();
									if (!materialBindings || materialBindings->GetShaderBindings().Num() == 0u)
									{
										bRangeComplete = false;
										continue;
									}

									RHIShaderBindingPtr materialBinding;
									if (materialBindings->GetShaderBindings().ContainsKey("material"))
									{
										materialBinding = materialBindings->GetShaderBindings()["material"];
									}
									for (size_t instanceIndex = 0u; instanceIndex < group.m_instanceTransforms.Num();
										++instanceIndex)
									{
										PerInstanceData data;
										data.model =
											proxy.ResolveInstancedMeshWorldMatrix(group, instanceIndex, meshIndex);
										data.skeletonOffset = proxy.GetSkeletonOffset();
										data.materialInstance =
											materialBinding ? materialBinding->GetStorageInstanceIndex() : 0u;
										data.bIsCulled = 0u;
										data.sphereBounds = mesh->m_bounds.ToSphere().GetVec4();
										data.bakedVolumeScale = vec4(mesh->m_bakedVolumeScale, 1.0f);
										rangeInstances.Add(std::move(data));
										rangeStableKeys.Add(BuildPackedDrawStableKey(proxy.m_handle,
											source->m_staticMeshEcs,
											static_cast<uint32_t>(groupIndex + 1u),
											static_cast<uint32_t>(meshIndex),
											static_cast<uint32_t>(instanceIndex)));
										AppendPackedDrawArenaMaterialVersion(
											rangeMaterialVersionRuns, batch.m_materialVersion);
									}
								}
							}

							if (!m_pagedArenaCache.ReplaceRange(rangeKey,
									rangeRevision,
									rangeInstances,
									rangeStableKeys,
									&rangeMaterialVersionRuns))
							{
								bRangeComplete = false;
							}
							bPayloadComplete[arenaPayloadIndex] &= bRangeComplete;
						});

					auto arenaPayload = m_pagedArenaCache.EndUpdate(bPayloadComplete[arenaPayloadIndex]);
					m_packet.UseSharedArenaPayload(mobility, std::move(arenaPayload));
					rangeInstances.Clear(false);
					rangeStableKeys.Clear(false);
					rangeMaterialVersionRuns.Clear(false);
					bBuildPayload[arenaPayloadIndex] = false;
				}
			}
			Details::EvictTextureBindingCache(m_textureBindingCache, sceneViewSnapshot.m_frame);
			m_packetPayloadCache.Evict(sceneViewSnapshot.m_frame);
			m_pagedArenaCache.Evict(sceneViewSnapshot.m_frame);

			SAILOR_PROFILE_SCOPE("Filter sceneView by tag");

			for (const auto& proxy : sceneViewSnapshot.m_proxies)
			{
				const auto* source = proxy.GetSource();
				if (!source)
				{
					continue;
				}
				const EMobilityType payloadMobility = bBackToFront ?
					EMobilityType::Dynamic : proxy.GetMobility();
				const size_t payloadIndex =
					RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(payloadMobility);
				if (!bBuildPayload[payloadIndex] && !usesPagedArena(payloadIndex))
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

					const auto mesh = proxy.ResolveMesh(i);
					const auto& material = source->GetMaterials()[i];

					const bool bRelevantMaterial = material &&
						material->GetRenderState().GetTag() == QueueTagHash;
					const bool bHasMaterialShaders = mesh && bRelevantMaterial &&
						material->GetVertexShader() &&
						material->GetFragmentShader();

					if (!bHasMaterialShaders)
					{
						if (bRelevantMaterial)
						{
							bPayloadComplete[payloadIndex] = false;
						}
						continue;
					}
					RHI::RHIBatch batch(material, mesh, materialSubmissionId);
					const auto materialBindings = batch.GetMaterialBindings();
					if (!materialBindings || materialBindings->GetShaderBindings().Num() == 0u)
					{
						bPayloadComplete[payloadIndex] = false;
						continue;
					}

					if (bRelevantMaterial)
					{
						RHIShaderBindingPtr shaderBinding;
						if (materialBindings->GetShaderBindings().ContainsKey("material"))
						{
							shaderBinding = materialBindings->GetShaderBindings()["material"];
						}

						uint32_t supportedMeshesPerBatch = (std::numeric_limits<uint32_t>::max)();
#if defined(__APPLE__)
						const auto& requestedTextures =
							source->m_materialTextureSamplers.Num() > i ?
							source->m_materialTextureSamplers[i] :
							Details::GetDefaultRequestedTextures();
						batch.m_textureBindings = Details::GetTextureBindingSet(
							m_textureBindingCache,
							requestedTextures,
							sceneViewSnapshot.m_frame,
							supportedMeshesPerBatch);
#else
						batch.m_textureBindings = App::GetSubmodule<TextureImporter>()->GetTextureSamplersBindingSet();
#endif
						batch.m_supportedMeshesPerBatch = supportedMeshesPerBatch;
						const uint64_t stableKey = BuildPackedDrawStableKey(
							proxy.m_handle,
							source->m_staticMeshEcs,
							0u,
							static_cast<uint32_t>(i),
							0u);
						if (usesPagedArena(payloadIndex))
						{
							if (!m_packet.AddArenaView(
								std::move(batch),
								mesh,
								BuildPackedDrawRangeKey(
									proxy.m_handle,
									source->m_staticMeshEcs,
									proxy.m_resource),
								stableKey,
								payloadMobility))
							{
								bPayloadComplete[payloadIndex] = false;
							}
							continue;
						}

						const glm::mat4 meshWorldMatrix = proxy.ResolveMeshWorldMatrix(i);
						RenderSceneNode::PerInstanceData data;
						data.model = meshWorldMatrix;
						data.skeletonOffset = proxy.GetSkeletonOffset();
						data.materialInstance = shaderBinding.IsValid() ?
							shaderBinding->GetStorageInstanceIndex() : 0u;
						data.bIsCulled = 0u;
						data.sphereBounds = mesh->m_bounds.ToSphere().GetVec4();
						data.bakedVolumeScale = vec4(mesh->m_bakedVolumeScale, 1.0f);

						if (bBackToFront)
						{
							const glm::vec4 worldCenter = meshWorldMatrix *
								glm::vec4(mesh->m_bounds.GetCenter(), 1.0f);
							const glm::vec4 viewCenter = sceneViewSnapshot.m_camera->GetViewMatrix() *
								worldCenter;

							OrderedDrawItem item;
							item.m_batch = std::move(batch);
							item.m_mesh = mesh;
							item.m_instanceData = data;
							item.m_cameraDepth = std::isfinite(viewCenter.z) ?
								-viewCenter.z :
								-(std::numeric_limits<float>::max)();
							item.m_staticMeshEcs = source->m_staticMeshEcs;
							item.m_meshIndex = i;
							m_orderedDrawItems.Emplace(std::move(item));
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

						const auto& sourceMesh = group.m_meshes[meshIndex];
						const auto& material = group.m_materials[meshIndex];
						const bool bRelevantMaterial = material &&
							material->GetRenderState().GetTag() == QueueTagHash;
						const bool bHasMaterialShaders = sourceMesh && bRelevantMaterial &&
							material->GetVertexShader() &&
							material->GetFragmentShader();
						if (!bHasMaterialShaders)
						{
							if (bRelevantMaterial)
							{
								bPayloadComplete[payloadIndex] = false;
							}
							continue;
						}

						RHI::RHIBatch batchTemplate(
							material,
							sourceMesh,
							materialSubmissionId);
						const auto materialBindings = batchTemplate.GetMaterialBindings();
						if (!materialBindings || materialBindings->GetShaderBindings().Num() == 0u)
						{
							bPayloadComplete[payloadIndex] = false;
							continue;
						}

						RHIShaderBindingPtr shaderBinding;
						if (materialBindings->GetShaderBindings().ContainsKey("material"))
						{
							shaderBinding = materialBindings->GetShaderBindings()["material"];
						}

						uint32_t supportedMeshesPerBatch = (std::numeric_limits<uint32_t>::max)();
#if defined(__APPLE__)
						const auto& requestedTextures =
							meshIndex < group.m_materialTextureSamplers.Num() ?
							group.m_materialTextureSamplers[meshIndex] :
							Details::GetDefaultRequestedTextures();
						batchTemplate.m_textureBindings = Details::GetTextureBindingSet(
							m_textureBindingCache,
							requestedTextures,
							sceneViewSnapshot.m_frame,
							supportedMeshesPerBatch);
#else
						batchTemplate.m_textureBindings = App::GetSubmodule<TextureImporter>()->GetTextureSamplersBindingSet();
#endif
						batchTemplate.m_supportedMeshesPerBatch = supportedMeshesPerBatch;

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
								bPayloadComplete[payloadIndex] = false;
								continue;
							}
							const uint64_t stableKey = BuildPackedDrawStableKey(
								proxy.m_handle,
								source->m_staticMeshEcs,
								static_cast<uint32_t>(groupIndex + 1u),
								static_cast<uint32_t>(meshIndex),
								static_cast<uint32_t>(instanceIndex));
							RHI::RHIBatch batch = batchTemplate;
							batch.m_mesh = mesh;
							if (usesPagedArena(payloadIndex))
							{
								if (!m_packet.AddArenaView(
									std::move(batch),
									mesh,
									BuildPackedDrawRangeKey(
										proxy.m_handle,
										source->m_staticMeshEcs,
										proxy.m_resource),
									stableKey,
									payloadMobility))
								{
									bPayloadComplete[payloadIndex] = false;
								}
								continue;
							}

							const glm::mat4 meshWorldMatrix = proxy.ResolveInstancedMeshWorldMatrix(
								group,
								instanceIndex,
								meshIndex);
							RenderSceneNode::PerInstanceData data;
							data.model = meshWorldMatrix;
							data.skeletonOffset = proxy.GetSkeletonOffset();
							data.materialInstance = shaderBinding.IsValid() ?
								shaderBinding->GetStorageInstanceIndex() : 0u;
							data.bIsCulled = 0u;
							data.sphereBounds = mesh->m_bounds.ToSphere().GetVec4();
							data.bakedVolumeScale = vec4(mesh->m_bakedVolumeScale, 1.0f);

							if (bBackToFront)
							{
								const glm::vec4 worldCenter = meshWorldMatrix *
									glm::vec4(mesh->m_bounds.GetCenter(), 1.0f);
								const glm::vec4 viewCenter = sceneViewSnapshot.m_camera->GetViewMatrix() *
									worldCenter;

								OrderedDrawItem item;
								item.m_batch = std::move(batch);
								item.m_mesh = mesh;
								item.m_instanceData = data;
								item.m_cameraDepth = std::isfinite(viewCenter.z) ?
									-viewCenter.z :
									-(std::numeric_limits<float>::max)();
								item.m_staticMeshEcs = source->m_staticMeshEcs;
								item.m_meshIndex = source->m_meshes.Num() + instanceIndex;
								HashCombine(item.m_meshIndex, groupIndex, meshIndex);
								m_orderedDrawItems.Emplace(std::move(item));
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

			if (bBackToFront)
			{
				m_orderedDrawItems.Sort([](const OrderedDrawItem& lhs, const OrderedDrawItem& rhs)
					{
						if (lhs.m_cameraDepth != rhs.m_cameraDepth)
						{
							return lhs.m_cameraDepth > rhs.m_cameraDepth;
						}

						if (lhs.m_staticMeshEcs != rhs.m_staticMeshEcs)
						{
							return lhs.m_staticMeshEcs < rhs.m_staticMeshEcs;
						}

						return lhs.m_meshIndex < rhs.m_meshIndex;
					});
				for (auto& item : m_orderedDrawItems)
				{
					m_packet.Add(std::move(item.m_batch), item.m_mesh, item.m_instanceData);
				}
				m_packet.Finalize(true);
			}
			else
			{
				m_packet.Finalize(false);
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
						bBuildPayload[index] && bPayloadComplete[index])
					{
						m_packetPayloadCache.Publish(
							buildPayloadCacheSlot(mobility),
							payloadRevisions[index],
							m_packet.SharePayload(mobility),
							sceneViewSnapshot.m_frame);
					}
				}
			}
			m_orderedDrawItems.Clear(false);
			syncSharedResources.Unlock();
		}, EThreadType::RHI);

	return res;
}

void RenderSceneNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
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
	if (resources->m_packet.GetNumInstances() == 0u)
	{
		m_syncSharedResources.Unlock();
		return;
	}

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	auto colorAttachment = GetRHIResource("color").DynamicCast<RHI::RHIRenderTarget>();
	auto colorSurface = GetRHIResource("color").DynamicCast<RHI::RHISurface>();
	if (colorSurface)
	{
		colorAttachment = colorSurface->GetTarget();
	}
	auto depthAttachment = GetRHIResource("depthStencil").DynamicCast<RHI::RHITexture>();
	if (!depthAttachment)
	{
		depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");
	}
	if (!colorAttachment || !depthAttachment)
	{
		m_syncSharedResources.Unlock();
		return;
	}

	RHIShaderBindingSetPtr nodeLightsData = sceneView.m_rhiLightsData;
	RHI::RHITexturePtr transmissionFramebuffer = GetResolvedAttachment("transmissionFramebuffer");
	if (transmissionFramebuffer)
	{
		constexpr const char* transmissionSamplerName = "g_transmissionFramebufferSampler";
		const uint64_t sourceRevision = sceneView.m_rhiLightsData ?
			sceneView.m_rhiLightsData->GetDescriptorRevision() : 0ull;
		const bool bCloneOutdated = !resources->m_nodeLightsBindings ||
			resources->m_nodeLightsSource != sceneView.m_rhiLightsData ||
			resources->m_nodeLightsSourceRevision != sourceRevision ||
			resources->m_transmissionTexture != transmissionFramebuffer;
		if (bCloneOutdated)
		{
			resources->m_nodeLightsBindings = CloneBindingsWithSampler(
				sceneView.m_rhiLightsData,
				transmissionSamplerName,
				transmissionFramebuffer,
				10u);
			resources->m_nodeLightsSource = sceneView.m_rhiLightsData;
			resources->m_nodeLightsSourceRevision = sourceRevision;
			resources->m_transmissionTexture = transmissionFramebuffer;
		}
		nodeLightsData = resources->m_nodeLightsBindings;
		commands->ImageMemoryBarrier(commandList, transmissionFramebuffer, RHI::EImageLayout::ShaderReadOnlyOptimal);
	}

	std::string gpuCullingSetting;
	TryGetString("GPUCulling", gpuCullingSetting);
	const bool bGpuCullingRequested = gpuCullingSetting == "true";
	std::string occlusionCullingSetting;
	TryGetString("OcclusionCulling", occlusionCullingSetting);
	const bool bOcclusionCullingRequested =
		bGpuCullingRequested && occlusionCullingSetting == "true";
	const uint32_t numInstances = resources->m_packet.GetNumStorageInstances();
	const uint32_t numInstanceIndices = resources->m_packet.GetNumDrawInstances();
	const size_t numAllocatedInstanceIndices =
		static_cast<size_t>(numInstanceIndices) * (bGpuCullingRequested ? 2u : 1u);
	if (!resources->m_perInstanceData ||
		resources->m_sizePerInstanceData < sizeof(PerInstanceData) * numInstances ||
		resources->m_sizeInstanceIndices < sizeof(uint32_t) * numAllocatedInstanceIndices)
	{
		resources->m_perInstanceData = driver->CreateShaderBindings();
		driver->AddSsboToShaderBindings(
			resources->m_perInstanceData,
			"data",
			sizeof(PerInstanceData),
			numInstances,
			0u);
		driver->AddSsboToShaderBindings(
			resources->m_perInstanceData,
			"indices",
			sizeof(uint32_t),
			numAllocatedInstanceIndices,
			1u);
		resources->m_sizePerInstanceData = sizeof(PerInstanceData) * numInstances;
		resources->m_sizeInstanceIndices =
			sizeof(uint32_t) * numAllocatedInstanceIndices;
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
				{ "OCCLUSION_CULLING" });
		}
	}

	RHIShaderPtr cullingShader;
	RHITexturePtr depthHighZ;
	if (bGpuCullingEnabled && m_pComputeMeshCullingShader && m_pComputeMeshCullingShader->IsReady())
	{
		depthHighZ = GetResolvedAttachment("depthHighZ").StaticCast<RHI::RHITexture>();
		if (depthHighZ)
		{
			if (!resources->m_computeMeshCullingBindings ||
				resources->m_cullingDepthHighZ != depthHighZ)
			{
				resources->m_computeMeshCullingBindings = driver->CreateShaderBindings();
				driver->AddSamplerToShaderBindings(
					resources->m_computeMeshCullingBindings,
					"depthHighZ",
					depthHighZ,
					0u);
				resources->m_cullingDepthHighZ = depthHighZ;
			}
#ifdef _DEBUG
			cullingShader = m_pComputeMeshCullingShader->GetDebugComputeShaderRHI();
#else
			cullingShader = m_pComputeMeshCullingShader->GetComputeShaderRHI();
#endif
		}
	}

	const auto viewport = glm::ivec4(
		0,
		colorAttachment->GetExtent().y,
		colorAttachment->GetExtent().x,
		-colorAttachment->GetExtent().y);
	const auto scissors = glm::uvec4(0, 0, colorAttachment->GetExtent().x, colorAttachment->GetExtent().y);
	const auto collectShaderBindings = [&](
		const RHIBatch& batch,
		TVector<RHIShaderBindingSetPtr>& sets)
		{
			sets.Add(sceneView.m_frameBindings);
			sets.Add(nodeLightsData);
			sets.Add(resources->m_perInstanceData);
			sets.Add(batch.GetMaterialBindings());
			sets.Add(batch.m_textureBindings);
			if (sceneView.m_boneMatrices)
			{
				sets.Add(sceneView.m_boneMatrices);
			}
		};

	commands->BeginDebugRegion(
		commandList,
		std::string(GetName()) + " QueueTag:" + GetString("Tag") + " Packed",
		DebugContext::Color_CmdGraphics);
	commands->ImageMemoryBarrier(commandList, colorAttachment, EImageLayout::ColorAttachmentOptimal);
	const auto depthLayout = RHI::IsDepthStencilFormat(depthAttachment->GetFormat()) ?
		EImageLayout::DepthStencilAttachmentOptimal : EImageLayout::DepthAttachmentOptimal;
	commands->ImageMemoryBarrier(commandList, depthAttachment, depthLayout);
	auto& renderPassColorAttachments =
		resources->m_renderPassColorAttachments;
	renderPassColorAttachments.Clear(false);
	renderPassColorAttachments.Add(colorAttachment);
	bool bRenderPassStarted = false;
	auto beginRenderPass = [&]()
		{
			commands->BeginRenderPass(
				commandList,
				renderPassColorAttachments,
				depthAttachment,
				glm::vec4(0, 0, colorAttachment->GetExtent().x, colorAttachment->GetExtent().y),
				glm::ivec2(0, 0),
				false,
				glm::vec4(0.0f),
				0.0f,
				true);
			bRenderPassStarted = true;
		};

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
	const bool bCurrentDepthOcclusionEnabled =
		bOcclusionCullingRequested && cullingShader && depthHighZ;
	if (bCurrentDepthOcclusionEnabled)
	{
		// Main-pass occlusion must execute on the graphics list after this frame's
		// depth pyramid. The flight upload list still completes before graphics.
		commands->ImageMemoryBarrierForComputeSampling(commandList, depthHighZ);
		m_drawCallStats = RHIRecordPackedDrawPacketWithCurrentDepthOcclusion(
			resources->m_packet,
			commandList,
			transferCommandList,
			collectShaderBindings,
			resources->m_perInstanceData,
			resources->m_indirectBuffers[0],
			viewport,
			scissors,
			glm::vec2(0.0f, 1.0f),
			cullingShader,
			&resources->m_cullingIndirectBufferBinding[0],
			cullingBindings,
			beginRenderPass);
	}
	else
	{
		beginRenderPass();
		m_drawCallStats = RHIRecordPackedDrawPacket(
			resources->m_packet,
			commandList,
			transferCommandList,
			collectShaderBindings,
			resources->m_perInstanceData,
			resources->m_indirectBuffers[0],
			viewport,
			scissors,
			glm::vec2(0.0f, 1.0f),
			cullingShader,
			&resources->m_cullingIndirectBufferBinding[0],
			cullingBindings);
	}

	if (bRenderPassStarted)
	{
		commands->EndRenderPass(commandList);
	}
	commands->EndDebugRegion(commandList);
	m_syncSharedResources.Unlock();
}

void RenderSceneNode::Clear()
{
#if defined(__APPLE__)
	m_textureBindingCache.Clear();
#endif
	m_packetPayloadCache.Clear();
	m_pagedArenaCache.Clear();
}
