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
#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/AssetRegistry.h"

#include <limits>
#include <mutex>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* RenderSceneNode::m_name = "RenderScene";
#endif

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

uint32_t RenderSceneNode::CalculatePlannedTextureSlotCount(const TVector<uint32_t>& requestedTextures)
{
	if (requestedTextures.IsEmpty())
	{
		return 1u;
	}

	const uint32_t maxTextureIndex = *std::max_element(requestedTextures.begin(), requestedTextures.end());
	return (std::max)(1u, maxTextureIndex + 1u);
}

RHI::ESortingOrder RenderSceneNode::GetSortingOrder() const
{
	const std::string& sortOrder = GetString("Sorting");

	if (!sortOrder.empty())
	{
		return magic_enum::enum_cast<RHI::ESortingOrder>(sortOrder).value_or(RHI::ESortingOrder::FrontToBack);
	}

	return RHI::ESortingOrder::FrontToBack;
}

RHIShaderBindingSetPtr RenderSceneNode::GetTextureBindingSet(const TSet<uint32_t>& requestedTextures, uint64_t frame, uint32_t& outSupportedMeshesPerBatch)
{
	auto textureImporter = App::GetSubmodule<TextureImporter>();
	if (!textureImporter)
	{
		outSupportedMeshesPerBatch = 1;
		return nullptr;
	}

	auto globalTextureSet = textureImporter->GetTextureSamplersBindingSet();

#if defined(__APPLE__)
	TextureBindingCacheKey key;
	key.m_requestedTextures = requestedTextures.ToVector();
	key.m_requestedTextures.RemoveAll([](uint32_t textureIndex)
		{
			return textureIndex >= TextureImporter::MaxTexturesInScene;
		});

	if (key.m_requestedTextures.IsEmpty())
	{
		key.m_requestedTextures.Add(0u);
	}

	if (!key.m_requestedTextures.Contains(0u))
	{
		key.m_requestedTextures.Add(0u);
	}

	key.m_requestedTextures.Sort();
	const uint64_t currentSourceDescriptorRevision = globalTextureSet ? globalTextureSet->GetDescriptorRevision() : 0;
	TextureBindingCacheEntry* cachedEntry = nullptr;
	m_textureBindingCache.Find(key, cachedEntry);

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

	TVector<RHITexturePtr> localTextures;
	const uint32_t plannedTextureSlots = CalculatePlannedTextureSlotCount(key.m_requestedTextures);
	localTextures.AddDefault(plannedTextureSlots);

	RHITexturePtr defaultTexture = driver->GetDefaultTexture();

	for (uint32_t textureIndex = 0; textureIndex < localTextures.Num(); textureIndex++)
	{
		localTextures[textureIndex] = defaultTexture;
	}

	for (const auto& slot : sourceSnapshot.m_slots)
	{
		if (slot.m_index < localTextures.Num() && slot.m_texture.IsValid())
		{
			localTextures[slot.m_index] = slot.m_texture;
		}
	}

	RHIShaderBindingSetPtr localTextureSet = driver->CreateShaderBindings();

	driver->AddSamplerToShaderBindings(localTextureSet, "textureSamplers", localTextures, 0, true, plannedTextureSlots);
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
		check(actualTextureCount == plannedTextureSlots);
		check(actualLayoutCount == plannedTextureSlots);
	}
#endif

	auto& entry = m_textureBindingCache[key];
	entry.m_textureBindings = localTextureSet;
	entry.m_textureSetSize = plannedTextureSlots;
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

void RenderSceneNode::EvictTextureBindingCache(uint64_t frame)
{
	TVector<TextureBindingCacheKey> expiredEntries;

	for (const auto& entry : m_textureBindingCache)
	{
		if (frame > entry.Second()->m_lastUsedFrame && frame - entry.Second()->m_lastUsedFrame > MaxTextureBindingCacheUnusedFrames)
		{
			expiredEntries.Add(entry.First());
		}
	}

	for (const auto& key : expiredEntries)
	{
		m_textureBindingCache.Remove(key);
	}
}

Tasks::TaskPtr<void, void> RenderSceneNode::Prepare(RHI::RHIFrameGraphPtr frameGraph, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	const std::string QueueTag = GetString("Tag");
	const size_t QueueTagHash = GetHash(QueueTag);

	auto res = Tasks::CreateTask("Prepare RenderSceneNode  " + std::to_string(sceneView.m_frame),
		[=, this, holdRhiResources = frameGraph, &syncSharedResources = m_syncSharedResources, &sceneViewSnapshot = sceneView]() mutable {

			syncSharedResources.Lock();

			m_numMeshes = 0;
			m_drawCalls.Clear();
			m_batches.Clear();
			EvictTextureBindingCache(sceneViewSnapshot.m_frame);

			SAILOR_PROFILE_SCOPE("Filter sceneView by tag");

			for (auto& proxy : sceneViewSnapshot.m_proxies)
			{
				for (size_t i = 0; i < proxy.m_meshes.Num(); i++)
				{
					const bool bHasMaterial = proxy.GetMaterials().Num() > i;
					if (!bHasMaterial)
					{
						break;
					}

					const auto& mesh = proxy.m_meshes[i];
					const auto& material = proxy.GetMaterials()[i];

					const bool bIsMaterialReady = material &&
						material->GetVertexShader() &&
						material->GetFragmentShader() &&
						material->GetBindings() &&
						material->GetBindings()->GetShaderBindings().Num() > 0;

					if (!bIsMaterialReady)
					{
						continue;
					}

					if (material->GetRenderState().GetTag() == QueueTagHash)
					{
						RHIShaderBindingPtr shaderBinding;
						if (material->GetBindings()->GetShaderBindings().ContainsKey("material"))
						{
							shaderBinding = material->GetBindings()->GetShaderBindings()["material"];
						}

						RenderSceneNode::PerInstanceData data;
						data.model = proxy.m_worldMatrix;
						data.skeletonOffset = proxy.m_skeletonOffset;
						data.materialInstance = shaderBinding.IsValid() ? shaderBinding->GetStorageInstanceIndex() : 0;
						data.bIsCulled = 0;
						data.sphereBounds = mesh->m_bounds.ToSphere().GetVec4();

						RHI::RHIBatch batch(material, mesh);
						uint32_t supportedMeshesPerBatch = (std::numeric_limits<uint32_t>::max)();
#if defined(__APPLE__)
						TSet<uint32_t> requestedTextures;
						if (proxy.m_materialTextureSamplers.Num() > i)
						{
							requestedTextures = proxy.m_materialTextureSamplers[i];
						}
						else
						{
							requestedTextures.Insert(0u);
						}
						batch.m_textureBindings = GetTextureBindingSet(requestedTextures, sceneViewSnapshot.m_frame, supportedMeshesPerBatch);
#else
						batch.m_textureBindings = App::GetSubmodule<TextureImporter>()->GetTextureSamplersBindingSet();
#endif
						batch.m_supportedMeshesPerBatch = supportedMeshesPerBatch;

						m_drawCalls[batch][mesh].Add(data);
						m_batches.Insert(batch);

						m_numMeshes++;
					}
				}
			}

			syncSharedResources.Unlock();
		}, EThreadType::RHI);

	return res;
}

/*
https://developer.nvidia.com/vulkan-shader-resource-binding
*/
void RenderSceneNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	m_drawCallStats = {};
	m_syncSharedResources.Lock();

	std::string temp;
	TryGetString("GPUCulling", temp);
	const bool bGpuCullingRequested = temp == "true";
	bool bGpuCullingEnabled = bGpuCullingRequested;

	const std::string QueueTag = GetString("Tag");

	auto scheduler = App::GetSubmodule<Tasks::Scheduler>();
	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	if (bGpuCullingEnabled)
	{
		if (!m_pComputeMeshCullingShader)
		{
			if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeMeshCulling.shader"))
			{
				App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetFileId(), m_pComputeMeshCullingShader, { "OCCLUSION_CULLING" });
			}
		}

		bGpuCullingEnabled = m_pComputeMeshCullingShader && m_pComputeMeshCullingShader->IsReady();

		RHI::RHITexturePtr depthHighZ;
		if (bGpuCullingEnabled)
		{
			depthHighZ = GetResolvedAttachment("depthHighZ").StaticCast<RHI::RHITexture>();
			bGpuCullingEnabled = depthHighZ.IsValid();
		}

		if (bGpuCullingEnabled && !m_computeMeshCullingBindings.IsValid())
		{
			m_computeMeshCullingBindings = driver->CreateShaderBindings();
			driver->AddSamplerToShaderBindings(m_computeMeshCullingBindings, "depthHighZ", depthHighZ, 0);
		}
	}

	if (m_numMeshes == 0)
	{
		m_syncSharedResources.Unlock();
		return;
	}

	if (!m_perInstanceData || m_sizePerInstanceData < sizeof(RenderSceneNode::PerInstanceData) * m_numMeshes)
	{
		SAILOR_PROFILE_SCOPE("Create storage for matrices");

		m_perInstanceData = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
		Sailor::RHI::Renderer::GetDriver()->AddSsboToShaderBindings(m_perInstanceData, "data", sizeof(RenderSceneNode::PerInstanceData), m_numMeshes, 0);
		m_sizePerInstanceData = sizeof(RenderSceneNode::PerInstanceData) * m_numMeshes;
	}

	RHI::RHIShaderBindingPtr storageBinding = m_perInstanceData->GetOrAddShaderBinding("data");

	{
		SAILOR_PROFILE_SCOPE("Prepare command list");
		TVector<PerInstanceData> gpuMatricesData;
		gpuMatricesData.AddDefault(m_numMeshes);

		RHI::RHISurfacePtr colorAttachment = GetRHIResource("color").DynamicCast<RHI::RHISurface>();
		RHI::RHITexturePtr depthAttachment = GetRHIResource("depthStencil").DynamicCast<RHI::RHITexture>();
		if (!depthAttachment)
		{
			depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");
		}

		if (!colorAttachment)
		{
			m_syncSharedResources.Unlock();
			return;
		}

		const auto viewport = glm::ivec4(0, colorAttachment->GetTarget()->GetExtent().y, colorAttachment->GetTarget()->GetExtent().x, -colorAttachment->GetTarget()->GetExtent().y);
		const auto scissor = glm::uvec4(0, 0, colorAttachment->GetTarget()->GetExtent().x, colorAttachment->GetTarget()->GetExtent().y);

		RHI::RHIShaderPtr cullingComputeShader;
		if (bGpuCullingEnabled)
		{
#ifdef _DEBUG
			cullingComputeShader = m_pComputeMeshCullingShader->GetDebugComputeShaderRHI();
#else
			cullingComputeShader = m_pComputeMeshCullingShader->GetComputeShaderRHI();
#endif
			bGpuCullingEnabled = cullingComputeShader.IsValid();
		}

		const size_t numThreads = scheduler->GetNumRHIThreads() + 1;
		const size_t materialsPerThread = (m_batches.Num()) / numThreads;

		if (m_indirectBuffers.Num() < numThreads)
		{
			m_indirectBuffers.Resize(numThreads);
			m_cullingIndirectBufferBinding.Resize(numThreads);

			for (uint32_t i = 0; i < numThreads; i++)
			{
				m_cullingIndirectBufferBinding[i] = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
			}
		}
		if (bGpuCullingEnabled)
		{
			bGpuCullingEnabled =
				m_computeMeshCullingBindings.IsValid() && m_computeMeshCullingBindings->IsReady() &&
				m_perInstanceData.IsValid() && m_perInstanceData->IsReady() &&
				!m_cullingIndirectBufferBinding.IsEmpty() && m_cullingIndirectBufferBinding[0].IsValid() && m_cullingIndirectBufferBinding[0]->IsReady() &&
				sceneView.m_frameBindings.IsValid() && sceneView.m_frameBindings->IsReady();
		}

		TVector<RHICommandListPtr> secondaryCommandLists(m_batches.Num() > numThreads ? (numThreads - 1) : 0);
		TVector<RHI::DrawCallStats> secondaryDrawCallStats(secondaryCommandLists.Num());
		TVector<Tasks::ITaskPtr> tasks;

		auto vecBatches = m_batches.ToVector();
		TVector<uint32_t> storageIndex(vecBatches.Num());
		TMap<RHIBatch, TVector<RHIShaderBindingSetPtr>> shaderBindingsCache;

		{
			SAILOR_PROFILE_SCOPE("Calculate SSBO offsets");

			size_t ssboIndex = 0;
			for (uint32_t j = 0; j < vecBatches.Num(); j++)
			{
				bool bIsInited = false;
				for (const auto& instancedDrawCall : m_drawCalls[vecBatches[j]])
				{
					auto& matrices = *instancedDrawCall.Second();

					memcpy(&gpuMatricesData[ssboIndex], matrices.GetData(), sizeof(PerInstanceData) * matrices.Num());

					if (!bIsInited)
					{
						storageIndex[j] = storageBinding->GetStorageInstanceIndex() + (uint32_t)ssboIndex;
						bIsInited = true;
					}
					ssboIndex += matrices.Num();
				}
			}
		}

		{
			SAILOR_PROFILE_SCOPE("Build shader bindings cache");

			for (const auto& batch : vecBatches)
			{
				TVector<RHIShaderBindingSetPtr> sets({ sceneView.m_frameBindings, sceneView.m_rhiLightsData, m_perInstanceData, batch.m_material->GetBindings(), batch.m_textureBindings });
				if (sceneView.m_boneMatrices)
				{
					sets.Add(sceneView.m_boneMatrices);
				}

				shaderBindingsCache.Insert(batch, std::move(sets));
			}
		}

		auto shaderBindingsByMaterial = [&](const RHIBatch& batch)
			{
				if (shaderBindingsCache.ContainsKey(batch))
				{
					return shaderBindingsCache[batch];
				}

				TVector<RHIShaderBindingSetPtr> sets({ sceneView.m_frameBindings, sceneView.m_rhiLightsData, m_perInstanceData, batch.m_material->GetBindings(), batch.m_textureBindings });
				if (sceneView.m_boneMatrices)
				{
					sets.Add(sceneView.m_boneMatrices);
				}
				return sets;
			};

		if (gpuMatricesData.Num() > 0)
		{
			SAILOR_PROFILE_SCOPE("Fill transfer command list with matrices data");
			commands->UpdateShaderBinding(transferCommandList, storageBinding,
				gpuMatricesData.GetData(),
				sizeof(PerInstanceData) * gpuMatricesData.Num(),
				0);
		}

		commands->BeginDebugRegion(commandList, std::string(GetName()) + " QueueTag:" + QueueTag, DebugContext::Color_CmdGraphics);
		std::mutex transferCommandListMutex;

		for (uint32_t i = 0; i < secondaryCommandLists.Num(); i++)
		{
			SAILOR_PROFILE_SCOPE("Create secondary command list");

			const uint32_t start = (uint32_t)materialsPerThread * i;
			const uint32_t end = (uint32_t)materialsPerThread * (i + 1);

			auto task = Tasks::CreateTask("Record draw calls in secondary command list",
				[&, i = i, start = start, end = end, transferCommandList = transferCommandList]()
				{
					RHICommandListPtr cmdList = driver->CreateCommandList(true, RHI::ECommandListQueue::Graphics);
					RHI::Renderer::GetDriver()->SetDebugName(cmdList, "Record draw calls in secondary command list");
					commands->BeginSecondaryCommandList(cmdList, true, true);

					const bool bCanDispatchCulling = bGpuCullingEnabled && cullingComputeShader.IsValid() &&
				m_computeMeshCullingBindings.IsValid() && m_computeMeshCullingBindings->IsReady() &&
				m_perInstanceData.IsValid() && m_perInstanceData->IsReady() &&
				m_cullingIndirectBufferBinding[i + 1].IsValid() && m_cullingIndirectBufferBinding[i + 1]->IsReady() &&
				sceneView.m_frameBindings.IsValid() && sceneView.m_frameBindings->IsReady();

					if (bCanDispatchCulling)
					{
						secondaryDrawCallStats[i] = RHIRecordDrawCallGPUCulling(start,
							end,
							vecBatches,
							cmdList, transferCommandList,
							shaderBindingsByMaterial,
							m_drawCalls,
							storageIndex,
							m_indirectBuffers[i + 1],
							viewport,
							scissor,
							glm::vec2(0.0f, 1.0f),
							cullingComputeShader, m_cullingIndirectBufferBinding[i + 1],
							{ m_computeMeshCullingBindings , m_perInstanceData, m_cullingIndirectBufferBinding[i + 1], sceneView.m_frameBindings },
							&transferCommandListMutex);
					}
					else
					{
						secondaryDrawCallStats[i] = RHIRecordDrawCall(start,
							end,
							vecBatches,
							cmdList, transferCommandList,
							shaderBindingsByMaterial,
							m_drawCalls,
							storageIndex,
							m_indirectBuffers[i + 1],
							viewport,
							scissor,
							glm::vec2(0.0f, 1.0f),
							&transferCommandListMutex);
					}

					commands->EndCommandList(cmdList);
					secondaryCommandLists[i] = std::move(cmdList);
				}, EThreadType::RHI);

			task->Run();
			tasks.Add(task);
		}

		commands->ImageMemoryBarrier(commandList, colorAttachment->GetTarget(), EImageLayout::ColorAttachmentOptimal);

		const auto depthAttachmentLayout = RHI::IsDepthStencilFormat(depthAttachment->GetFormat()) ? EImageLayout::DepthStencilAttachmentOptimal : EImageLayout::DepthAttachmentOptimal;
		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachmentLayout);

		if (m_batches.Num() > 0)
		{
			SAILOR_PROFILE_SCOPE("Record draw calls in primary command list");
			commands->BeginRenderPass(commandList,
				TVector<RHI::RHISurfacePtr>{ colorAttachment },
				depthAttachment,
				glm::vec4(0, 0, colorAttachment->GetTarget()->GetExtent().x, colorAttachment->GetTarget()->GetExtent().y),
				glm::ivec2(0, 0),
				false,
				glm::vec4(0.0f),
				0.0f,
				true);

			const bool bCanDispatchCulling = bGpuCullingEnabled && cullingComputeShader.IsValid() &&
			m_computeMeshCullingBindings.IsValid() && m_computeMeshCullingBindings->IsReady() &&
			m_perInstanceData.IsValid() && m_perInstanceData->IsReady() &&
			m_cullingIndirectBufferBinding[0].IsValid() && m_cullingIndirectBufferBinding[0]->IsReady() &&
			sceneView.m_frameBindings.IsValid() && sceneView.m_frameBindings->IsReady();

			if (bCanDispatchCulling)
			{
				m_drawCallStats += RHIRecordDrawCallGPUCulling((uint32_t)secondaryCommandLists.Num() * (uint32_t)materialsPerThread,
					(uint32_t)vecBatches.Num(),
					vecBatches,
					commandList, transferCommandList,
					shaderBindingsByMaterial,
					m_drawCalls,
					storageIndex,
					m_indirectBuffers[0],
					viewport,
					scissor,
					glm::vec2(0.0f, 1.0f),
					cullingComputeShader, m_cullingIndirectBufferBinding[0],
					{ m_computeMeshCullingBindings , m_perInstanceData, m_cullingIndirectBufferBinding[0], sceneView.m_frameBindings },
					&transferCommandListMutex);
			}
			else
			{
				m_drawCallStats += RHIRecordDrawCall((uint32_t)secondaryCommandLists.Num() * (uint32_t)materialsPerThread,
					(uint32_t)vecBatches.Num(),
					vecBatches,
					commandList, transferCommandList,
					shaderBindingsByMaterial,
					m_drawCalls,
					storageIndex,
					m_indirectBuffers[0],
					viewport,
					scissor,
					glm::vec2(0.0f, 1.0f),
					&transferCommandListMutex);
			}

			commands->EndRenderPass(commandList);
		}

		{
			SAILOR_PROFILE_SCOPE("Wait for secondary command lists");
			for (auto& task : tasks)
			{
				task->Wait();
			}

			for (const RHI::DrawCallStats& drawCallStats : secondaryDrawCallStats)
			{
				m_drawCallStats += drawCallStats;
			}
		}

		if (secondaryCommandLists.Num() > 0)
		{
			commands->RenderSecondaryCommandBuffers(commandList,
				secondaryCommandLists,
				TVector<RHI::RHISurfacePtr>{ colorAttachment },
				depthAttachment,
				glm::vec4(0, 0, colorAttachment->GetTarget()->GetExtent().x, colorAttachment->GetTarget()->GetExtent().y),
				glm::ivec2(0, 0),
				false,
				glm::vec4(0.0f),
				0.0f,
				true);
		}

	}
	commands->EndDebugRegion(commandList);

	m_syncSharedResources.Unlock();
}

void RenderSceneNode::Clear()
{
	m_indirectBuffers.Clear();
	m_perInstanceData.Clear();
#if defined(__APPLE__)
	m_textureBindingCache.Clear();
#endif
}
