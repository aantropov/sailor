#include "RenderSceneNode.h"
#include "RHI/MaterialPreparationCache.h"
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
#include "Core/StringHash.h"
#include "Core/SpinLock.h"

#include <cmath>
#include <limits>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* RenderSceneNode::m_name = "RenderScene";
#endif

namespace
{
	RHIObjectMotionData ResolveMeshMotion(const RHISceneViewSnapshot& view,
		const RHIVisibleSceneProxy& proxy, size_t meshIndex,
		const RHIInstancedMeshGroup* group = nullptr, size_t instanceIndex = 0u)
	{
		RHIVisibleSceneProxy previous;
		const bool valid = ResolvePreviousMotionProxy(view, proxy, previous);
		const auto currentModel = group ? proxy.ResolveInstancedMeshWorldMatrix(*group, instanceIndex, meshIndex) : proxy.ResolveMeshWorldMatrix(meshIndex);
		const auto previousModel = valid ? (group ? previous.ResolveInstancedMeshWorldMatrix(*group, instanceIndex, meshIndex) : previous.ResolveMeshWorldMatrix(meshIndex)) : currentModel;
		return MakeObjectMotionData(currentModel, previousModel, valid ? previous.GetSkeletonOffset() : 0xFFFFFFFFu, valid);
	}

	bool TryPrepareBatch(const RHIMaterialPtr& material, const RHIMeshPtr& mesh,
		RHIMaterialPreparationCache& preparedMaterials, RHIBatch& batch)
	{
		if (!mesh || !preparedMaterials.Get(material).m_bHasGraphicsShaders)
		{
			return false;
		}
		batch = preparedMaterials.MakeBatch(material, mesh);
		const auto* bindings = batch.GetMaterialBindingsRaw();
		return bindings && !bindings->GetShaderBindings().IsEmpty();
	}

	RenderSceneNode::PerInstanceData MakeInstanceData(
		const RHISceneViewSnapshot& view, const RHIVisibleSceneProxy& proxy,
		const RHIMesh& mesh, const RHIMaterialPreparationCache::Entry& material,
		size_t meshIndex, const RHIInstancedMeshGroup* group = nullptr, size_t instanceIndex = 0)
	{
		RenderSceneNode::PerInstanceData data;
		data.model = group ? proxy.ResolveInstancedMeshWorldMatrix(*group, instanceIndex, meshIndex) :
			proxy.ResolveMeshWorldMatrix(meshIndex);
		data.motion = ResolveMeshMotion(view, proxy, meshIndex, group, instanceIndex);
		data.motion.m_state.z = material.m_material->GetRenderState().GetBlendMode() != EBlendMode::None;
		data.skeletonOffset = proxy.GetSkeletonOffset();
		data.materialInstance = material.m_materialInstance;
		data.sphereBounds = mesh.m_bounds.ToSphere().GetVec4();
		data.bakedVolumeScale = vec4(mesh.m_bakedVolumeScale, 1.0f);
		return data;
	}

	void HashMotionHistory(size_t& revision, const RHISceneViewSnapshot& view, const RHIVisibleSceneProxy& proxy)
	{
		RHIVisibleSceneProxy previous;
		const bool valid = ResolvePreviousMotionProxy(view, proxy, previous);
		HashCombine(revision, valid);
		if (valid)
			HashCombine(revision, std::hash<glm::mat4>{}(previous.GetWorldMatrix()), previous.GetSkeletonOffset());
	}

	RHITexturePtr GetBoundTexture(
		const RHIShaderBindingSetPtr& bindings,
		const std::string& samplerName)
	{
		if (!bindings)
		{
			return nullptr;
		}

		const auto& shaderBindings = bindings->GetShaderBindings();
		const auto bindingIt = shaderBindings.Find(samplerName);
		if (bindingIt == shaderBindings.end() || !bindingIt->m_second)
		{
			return nullptr;
		}

		return bindingIt->m_second->GetTextureBinding();
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
	const TVector<uint32_t>& requestedTextures,
	uint64_t frame,
	uint32_t& outSupportedMeshesPerBatch,
	bool& outCurrent)
{
	outCurrent = false;
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
		outCurrent = true;
		return cachedEntry->m_textureBindings;
	}

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	const auto sourceSnapshot = textureImporter->GetTextureSamplersSnapshot(requestedTextures);
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
		outCurrent = true;
		return cachedEntry->m_textureBindings;
	}

	auto getFallbackBindings = [&]() -> RHIShaderBindingSetPtr
		{
			if (cachedEntry)
			{
				cachedEntry->m_lastUsedFrame = frame;
				outSupportedMeshesPerBatch = (std::max)(1u, MaxTextureSlotsPerBatch / (std::max)(1u, cachedEntry->m_textureSetSize));
				return cachedEntry->m_textureBindings;
			}

			outSupportedMeshesPerBatch = 1u;
			return nullptr;
		};

	RHITexturePtr defaultTexture = driver->GetDefaultTexture();
	TVector<RHITexturePtr> localTextures{ defaultTexture };
	TVector<uint32_t> globalToLocal =
		Details::BuildDenseTextureRemap(requestedTextures);
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
		return getFallbackBindings();
	}
	memcpy(remapBuffer->GetPointer(), globalToLocal.GetData(), remapBufferSize);

	if (!driver->AddBufferToShaderBindings(
		localTextureSet,
		remapBuffer,
		"textureSamplerRemap",
		0))
	{
		return getFallbackBindings();
	}
	if (!driver->AddSamplerToShaderBindings(
		localTextureSet,
		"textureSamplers",
		localTextures,
		1,
		true,
		denseTextureCount))
	{
		return getFallbackBindings();
	}
	localTextureSet->RecalculateCompatibility();

#if defined(SAILOR_BUILD_WITH_VULKAN)
	if (!localTextureSet->m_vulkan.m_descriptorSet || !localTextureSet->m_vulkan.m_descriptorSet->IsCompiled())
	{
		return getFallbackBindings();
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

	if (!cachedEntry)
	{
		key.Materialize();
	}
	auto& entry = cachedEntry ? *cachedEntry : textureBindingCache[key];
	entry.m_textureBindings = localTextureSet;
	entry.m_textureRemapBuffer = remapBuffer;
	entry.m_textureSetSize = denseTextureCount;
	entry.m_lastUsedFrame = frame;
	entry.m_sourceDescriptorRevision = sourceSnapshot.m_descriptorRevision;
	entry.m_sourceSlotRevisions = std::move(currentSlotRevisions);

	outSupportedMeshesPerBatch = (std::max)(1u, MaxTextureSlotsPerBatch / (std::max)(1u, entry.m_textureSetSize));
	outCurrent = true;
	return entry.m_textureBindings;
#else
	outSupportedMeshesPerBatch = (std::numeric_limits<uint32_t>::max)();
	outCurrent = globalTextureSet.IsValid();
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
	const size_t QueueTagHash = StringHash::Runtime(QueueTag).GetHash();
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
			RHIMaterialPreparationCache preparedMaterials(sceneViewSnapshot.m_submissionContext->GetSubmissionId());

			auto submissionResources = sceneViewSnapshot.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(
				this,
				sceneViewSnapshot.m_cameraIndex,
				0u);
			auto& packet = submissionResources->m_packet;
			auto& orderedDrawItems = submissionResources->m_orderedDrawItems;

			packet.Reset();
			orderedDrawItems.Clear(false);
			const bool bUsesPagedArenas = bVirtualizeInstancePayloads && !bBackToFront;
			syncSharedResources.Lock();
			if (bUsesPagedArenas)
			{
				BuildStableArenas(sceneViewSnapshot, *submissionResources, preparedMaterials, QueueTagHash);
			}
			Details::EvictTextureBindingCache(m_textureBindingCache, sceneViewSnapshot.m_frame);
			m_pagedArenaCache.Evict(sceneViewSnapshot.m_frame);
			BuildVisiblePacket(sceneViewSnapshot, *submissionResources, preparedMaterials, QueueTagHash,
				bUsesPagedArenas, bBackToFront);
			syncSharedResources.Unlock();

			if (bBackToFront)
			{
				orderedDrawItems.Sort([](const OrderedDrawItem& lhs, const OrderedDrawItem& rhs)
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
				for (auto& item : orderedDrawItems)
				{
					packet.Add(std::move(item.m_batch), item.m_mesh, item.m_instanceData);
				}
				packet.Finalize(true);
			}
			else
			{
				packet.Finalize(false);
			}
			orderedDrawItems.Clear(false);
		}, EThreadType::Worker);

	return res;
}

void RenderSceneNode::BuildStableArenas(const RHISceneViewSnapshot& sceneView,
	SubmissionResources& resources, RHIMaterialPreparationCache& preparedMaterials, size_t queueTagHash)
{
	SAILOR_PROFILE_SCOPE("Build main stable arenas");
	auto& packet = resources.m_packet;
	const uint64_t materialSubmissionId = sceneView.m_submissionContext->GetSubmissionId();
	for (const EMobilityType mobility : { EMobilityType::Static, EMobilityType::Stationary })
	{
		const size_t arenaPayloadIndex = TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(mobility);
		size_t payloadRevision = Fnv1aOffsetBasis;
		HashCombine(payloadRevision, arenaPayloadIndex, queueTagHash,
			sceneView.m_submissionContext->GetMaterialRevision(), sceneView.GetMobilityRevision(mobility));
		HashCombine(payloadRevision, sceneView.m_previousMotionFrame ?
			sceneView.m_previousMotionFrame->m_mobilityRevisions[static_cast<size_t>(mobility)] : 0ull);
		size_t arenaCacheSlot = Fnv1aOffsetBasis;
		HashCombine(arenaCacheSlot, 0u, arenaPayloadIndex);
		if (auto payload = m_pagedArenaCache.Find(arenaCacheSlot, payloadRevision, sceneView.m_frame))
		{
			packet.UseSharedArenaPayload(mobility, std::move(payload));
			continue;
		}
		m_pagedArenaCache.BeginUpdate(
			arenaCacheSlot, payloadRevision, sceneView.m_frame);
		auto& rangeInstances = resources.m_arenaRangeInstances;
		auto& rangeStableKeys = resources.m_arenaRangeStableKeys;
		auto& rangeMaterialVersionRuns = resources.m_arenaRangeMaterialVersionRuns;
		bool bPayloadComplete = true;
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
					proxy.m_resource ? proxy.m_resource->m_mainRevision : proxy.GetContentRevision();
				HashCombine(rangeRevision,
					queueTagHash,
					proxy.GetContentRevision(),
					std::hash<glm::mat4>{}(proxy.GetWorldMatrix()),
					proxy.GetSkeletonOffset());
				if (proxy.m_record)
				{
					HashMotionHistory(rangeRevision, sceneView, proxy);
				}
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
						material && material->GetRenderState().GetTag() == queueTagHash;
					if (!bRelevantMaterial)
					{
						continue;
					}
					RHIBatch batch;
					if (!TryPrepareBatch(material, mesh, preparedMaterials, batch))
					{
						bRangeComplete = false;
						continue;
					}

					const auto& preparedMaterial = preparedMaterials.Get(material);
					auto data = MakeInstanceData(
						sceneView, proxy, *mesh, preparedMaterial, meshIndex);
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
							material && material->GetRenderState().GetTag() == queueTagHash;
						if (!bRelevantMaterial)
						{
							continue;
						}
						RHIBatch batch;
						if (!TryPrepareBatch(material, mesh, preparedMaterials, batch))
						{
							bRangeComplete = false;
							continue;
						}

						const auto& preparedMaterial = preparedMaterials.Get(material);
						for (size_t instanceIndex = 0u; instanceIndex < group.m_instanceTransforms.Num();
							++instanceIndex)
						{
							auto data = MakeInstanceData(
								sceneView, proxy, *mesh, preparedMaterial, meshIndex, &group, instanceIndex);
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
				bPayloadComplete &= bRangeComplete;
			});

		auto arenaPayload = m_pagedArenaCache.EndUpdate(bPayloadComplete);
		packet.UseSharedArenaPayload(mobility, std::move(arenaPayload));
		rangeInstances.Clear(false);
		rangeStableKeys.Clear(false);
		rangeMaterialVersionRuns.Clear(false);
	}
}

void RenderSceneNode::BuildVisiblePacket(const RHISceneViewSnapshot& sceneView,
	SubmissionResources& resources, RHIMaterialPreparationCache& preparedMaterials, size_t queueTagHash,
	bool bUsesPagedArenas, bool bBackToFront)
{
	auto& packet = resources.m_packet;
	auto& orderedDrawItems = resources.m_orderedDrawItems;
	SAILOR_PROFILE_SCOPE("Build visible main packet");

	for (const auto& proxy : sceneView.m_proxies)
	{
		const auto* source = proxy.GetSource();
		if (!source)
		{
			continue;
		}
		const EMobilityType payloadMobility = bBackToFront ?
			EMobilityType::Dynamic : proxy.GetMobility();
		const bool bArenaView = bUsesPagedArenas &&
			(payloadMobility == EMobilityType::Static || payloadMobility == EMobilityType::Stationary);
		for (size_t i = 0; i < source->m_meshes.Num(); i++)
		{
			const bool bHasMaterial = source->GetMaterials().Num() > i;
			if (!bHasMaterial)
			{
				break;
			}

			const auto& mesh = sceneView.ResolveMesh(proxy, i);
			const auto& material = source->GetMaterials()[i];

			const bool bRelevantMaterial = material &&
				material->GetRenderState().GetTag() == queueTagHash;
			if (!bRelevantMaterial)
			{
				continue;
			}
			RHIBatch batch;
			if (!TryPrepareBatch(material, mesh, preparedMaterials, batch))
			{
				continue;
			}

			const auto& preparedMaterial = preparedMaterials.Get(material);

			uint32_t supportedMeshesPerBatch = (std::numeric_limits<uint32_t>::max)();
#if defined(__APPLE__)
			bool bCurrentTextureBindings = false;
			const auto& requestedTextures =
				source->m_materialTextureSamplers.Num() > i ?
				source->m_materialTextureSamplers[i] :
				Details::GetDefaultRequestedTextures();
			batch.m_textureBindings = Details::GetTextureBindingSet(
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
			const uint64_t stableKey = BuildPackedDrawStableKey(
				proxy.m_handle,
				source->m_staticMeshEcs,
				0u,
				static_cast<uint32_t>(i),
				0u);
			if (bArenaView)
			{
				packet.AddArenaView(
					std::move(batch),
					mesh,
					BuildPackedDrawRangeKey(
						proxy.m_handle,
						source->m_staticMeshEcs,
						proxy.m_resource),
					stableKey,
					payloadMobility);
				continue;
			}

			auto data = MakeInstanceData(
				sceneView, proxy, *mesh, preparedMaterial, i);

			if (bBackToFront)
			{
				const glm::vec4 worldCenter = data.model *
					glm::vec4(mesh->m_bounds.GetCenter(), 1.0f);
				const glm::vec4 viewCenter = sceneView.m_camera->GetViewMatrix() *
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
				orderedDrawItems.Emplace(std::move(item));
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

				const auto& sourceMesh = group.m_meshes[meshIndex];
				const auto& material = group.m_materials[meshIndex];
				const bool bRelevantMaterial = material &&
					material->GetRenderState().GetTag() == queueTagHash;
				if (!bRelevantMaterial)
				{
					continue;
				}
				RHIBatch batchTemplate;
				if (!TryPrepareBatch(material, sourceMesh, preparedMaterials, batchTemplate))
				{
					continue;
				}

				const auto& preparedMaterial = preparedMaterials.Get(material);

				uint32_t supportedMeshesPerBatch = (std::numeric_limits<uint32_t>::max)();
#if defined(__APPLE__)
				bool bCurrentTextureBindings = false;
				const auto& requestedTextures =
					meshIndex < group.m_materialTextureSamplers.Num() ?
					group.m_materialTextureSamplers[meshIndex] :
					Details::GetDefaultRequestedTextures();
				batchTemplate.m_textureBindings = Details::GetTextureBindingSet(
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
					RHI::RHIBatch batch = batchTemplate;
					batch.m_mesh = mesh;
					if (bArenaView)
					{
						packet.AddArenaView(
							std::move(batch),
							mesh,
							BuildPackedDrawRangeKey(
								proxy.m_handle,
								source->m_staticMeshEcs,
								proxy.m_resource),
							stableKey,
							payloadMobility);
						continue;
					}

					auto data = MakeInstanceData(
						sceneView, proxy, *mesh, preparedMaterial, meshIndex, &group, instanceIndex);

					if (bBackToFront)
					{
						const glm::vec4 worldCenter = data.model *
							glm::vec4(mesh->m_bounds.GetCenter(), 1.0f);
						const glm::vec4 viewCenter = sceneView.m_camera->GetViewMatrix() *
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
						orderedDrawItems.Emplace(std::move(item));
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
	if (resources->m_packet.GetNumInstances() == 0u)
	{
		return;
	}

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	auto colorAttachment = GetTargetAttachment("color", frameGraph.GetRawPtr());
	auto colorSurface = GetRHIResource("color", frameGraph.GetRawPtr()).DynamicCast<RHI::RHISurface>();
	auto motionAttachment = GetTargetAttachment("motionVectors", frameGraph.GetRawPtr());
	auto motionSurface = GetRHIResource("motionVectors", frameGraph.GetRawPtr()).DynamicCast<RHI::RHISurface>();
	auto depthAttachment = GetResolvedAttachment("depthStencil", frameGraph.GetRawPtr());
	if (!depthAttachment)
	{
		depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");
	}
	if (!colorAttachment || !depthAttachment)
	{
		return;
	}

	RHI::RHITexturePtr transmissionFramebuffer = GetResolvedAttachment("transmissionFramebuffer", frameGraph.GetRawPtr());
	RHI::RHITexturePtr sceneDepth = GetResolvedAttachment("sceneDepth", frameGraph.GetRawPtr());
	RHI::RHITexturePtr sampledSceneDepth = GetSampledAttachment("sceneDepth", frameGraph.GetRawPtr());
	RHI::RHITexturePtr globalIlluminationProbeCellIndicesTexture =
		GetResolvedAttachment("globalIlluminationProbeCellIndicesSampler", frameGraph.GetRawPtr());
	if (transmissionFramebuffer)
	{
		commands->ImageMemoryBarrier(commandList, transmissionFramebuffer, RHI::EImageLayout::ShaderReadOnlyOptimal);
	}
	if (sceneDepth)
	{
		commands->ImageMemoryBarrier(commandList, sceneDepth, RHI::EImageLayout::ShaderReadOnlyOptimal);
	}
	if (globalIlluminationProbeCellIndicesTexture)
	{
		commands->ImageMemoryBarrier(
			commandList,
			globalIlluminationProbeCellIndicesTexture,
			RHI::EImageLayout::ShaderReadOnlyOptimal);
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

	// Pass inputs live with the instance data; lighting remains shared by the view.
	const auto bindPassTexture = [&](const char* name, RHITexturePtr texture, uint32_t binding)
	{
		if (!texture) texture = driver->GetDefaultTexture();
		return GetBoundTexture(resources->m_perInstanceData, name) == texture ||
			driver->AddSamplerToShaderBindings(resources->m_perInstanceData, name, texture, binding);
	};
	if (!bindPassTexture("g_transmissionFramebufferSampler", transmissionFramebuffer, 2u) ||
		!bindPassTexture("g_sceneDepthSampler", sampledSceneDepth, 3u) ||
		!bindPassTexture("g_globalIlluminationProbeCellIndicesSampler", globalIlluminationProbeCellIndicesTexture, 4u))
	{
		return;
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
		depthHighZ = GetResolvedAttachment("depthHighZ", frameGraph.GetRawPtr());
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
			sets.Add(sceneView.m_rhiLightsData);
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
	if (motionAttachment)
	{
		commands->ImageMemoryBarrier(commandList, motionAttachment, EImageLayout::ColorAttachmentOptimal);
		if (motionSurface && motionSurface->NeedsResolve())
			commands->ImageMemoryBarrier(commandList, motionSurface->GetResolved(), EImageLayout::ColorAttachmentOptimal);
	}
	if (colorSurface && colorSurface->NeedsResolve())
	{
		commands->ImageMemoryBarrier(
			commandList,
			colorSurface->GetResolved(),
			EImageLayout::ColorAttachmentOptimal);
	}
	const auto depthLayout = RHI::IsDepthStencilFormat(depthAttachment->GetFormat()) ?
		EImageLayout::DepthStencilAttachmentOptimal : EImageLayout::DepthAttachmentOptimal;
	commands->ImageMemoryBarrier(commandList, depthAttachment, depthLayout);
	bool bRenderPassStarted = false;
	const auto beginRenderPass = [&]()
		{
			const glm::vec4 renderArea(
				0,
				0,
				colorAttachment->GetExtent().x,
				colorAttachment->GetExtent().y);
			auto& attachments = resources->m_renderPassColorAttachments;
			auto& resolves = resources->m_renderPassColorResolves;
			attachments.Clear(false);
			resolves.Clear(false);
			attachments.Add(colorAttachment);
			resolves.Add(colorSurface && colorSurface->NeedsResolve() ? colorSurface->GetResolved() : nullptr);
			if (motionAttachment)
			{
				attachments.Add(motionAttachment);
				resolves.Add(motionSurface && motionSurface->NeedsResolve() ? motionSurface->GetResolved() : nullptr);
			}
			bRenderPassStarted = commands->BeginRenderPass(commandList, attachments, resolves, depthAttachment, renderArea,
				glm::ivec2(0), false, glm::vec4(0), 0.0f, !colorSurface || colorSurface->NeedsResolve(), true);
			return bRenderPassStarted;
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
		bOcclusionCullingRequested && cullingShader &&
		frameGraph->HasCurrentDepthPyramid(depthHighZ);
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
	else if (beginRenderPass())
	{
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
}

void RenderSceneNode::Clear()
{
#if defined(__APPLE__)
	m_textureBindingCache.Clear();
#endif
	m_pagedArenaCache.Clear();
}
