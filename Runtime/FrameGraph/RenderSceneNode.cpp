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

	auto res = Tasks::CreateTask("Prepare RenderSceneNode  " + std::to_string(sceneView.m_frame),
		[=, this, holdRhiResources = frameGraph, &syncSharedResources = m_syncSharedResources, &sceneViewSnapshot = sceneView]() mutable {

			syncSharedResources.Lock();

			m_numMeshes = 0;
			m_drawCalls.Clear();
			m_batches.Clear();
			m_orderedDrawItems.Clear();
			Details::EvictTextureBindingCache(m_textureBindingCache, sceneViewSnapshot.m_frame);

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
						data.bakedVolumeScale = vec4(mesh->m_bakedVolumeScale, 1.0f);

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
						batch.m_textureBindings = Details::GetTextureBindingSet(
							m_textureBindingCache,
							requestedTextures,
							sceneViewSnapshot.m_frame,
							supportedMeshesPerBatch);
#else
						batch.m_textureBindings = App::GetSubmodule<TextureImporter>()->GetTextureSamplersBindingSet();
#endif
						batch.m_supportedMeshesPerBatch = supportedMeshesPerBatch;

						if (bBackToFront)
						{
							const glm::vec4 worldCenter = proxy.m_worldMatrix *
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
							item.m_staticMeshEcs = proxy.m_staticMeshEcs;
							item.m_meshIndex = i;
							m_orderedDrawItems.Emplace(std::move(item));
						}
						else
						{
							m_drawCalls[batch][mesh].Add(data);
							m_batches.Insert(batch);
						}

						m_numMeshes++;
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
			}

			syncSharedResources.Unlock();
		}, EThreadType::RHI);

	return res;
}

void RenderSceneNode::ProcessBackToFront(RHIFrameGraphPtr frameGraph,
	RHI::RHICommandListPtr transferCommandList,
	RHI::RHICommandListPtr commandList,
	const RHI::RHISceneViewSnapshot& sceneView,
	const RHI::RHIShaderBindingPtr& storageBinding,
	const std::string& queueTag)
{
	if (m_orderedDrawItems.IsEmpty())
	{
		return;
	}

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	RHI::RHISurfacePtr colorAttachment = GetRHIResource("color").DynamicCast<RHI::RHISurface>();
	RHI::RHITexturePtr depthAttachment = GetRHIResource("depthStencil").DynamicCast<RHI::RHITexture>();
	if (!depthAttachment)
	{
		depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");
	}

	if (!colorAttachment || !depthAttachment)
	{
		return;
	}

	TVector<PerInstanceData> gpuMatricesData;
	gpuMatricesData.Reserve(m_orderedDrawItems.Num());

	TVector<RHI::DrawIndexedIndirectData> indirectCommands;
	indirectCommands.Reserve(m_orderedDrawItems.Num());

	const uint32_t firstStorageInstance = storageBinding->GetStorageInstanceIndex();
	for (uint32_t i = 0; i < m_orderedDrawItems.Num(); i++)
	{
		const OrderedDrawItem& item = m_orderedDrawItems[i];
		gpuMatricesData.Add(item.m_instanceData);

		RHI::DrawIndexedIndirectData command{};
		command.m_indexCount = item.m_mesh->GetIndexCount();
		command.m_instanceCount = 1u;
		command.m_firstIndex = item.m_mesh->GetFirstIndex();
		command.m_vertexOffset = item.m_mesh->GetVertexOffset();
		command.m_firstInstance = firstStorageInstance + i;
		indirectCommands.Emplace(std::move(command));
	}

	commands->UpdateShaderBinding(transferCommandList,
		storageBinding,
		gpuMatricesData.GetData(),
		sizeof(PerInstanceData) * gpuMatricesData.Num(),
		0);

	if (m_indirectBuffers.IsEmpty())
	{
		m_indirectBuffers.Resize(1);
	}

	const size_t indirectBufferSize =
		sizeof(RHI::DrawIndexedIndirectData) * indirectCommands.Num();
	if (!m_indirectBuffers[0].IsValid() ||
		m_indirectBuffers[0]->GetSize() < indirectBufferSize)
	{
		constexpr size_t IndirectBufferSlack = 256;
		m_indirectBuffers[0] = driver->CreateIndirectBuffer(
			indirectBufferSize + IndirectBufferSlack);
	}

	commands->UpdateBuffer(transferCommandList,
		m_indirectBuffers[0],
		indirectCommands.GetData(),
		indirectBufferSize,
		0);

	const auto viewport = glm::ivec4(0,
		colorAttachment->GetTarget()->GetExtent().y,
		colorAttachment->GetTarget()->GetExtent().x,
		-colorAttachment->GetTarget()->GetExtent().y);
	const auto scissor = glm::uvec4(0,
		0,
		colorAttachment->GetTarget()->GetExtent().x,
		colorAttachment->GetTarget()->GetExtent().y);

	commands->BeginDebugRegion(commandList,
		std::string(GetName()) + " QueueTag:" + queueTag + " BackToFront",
		DebugContext::Color_CmdGraphics);
	commands->ImageMemoryBarrier(commandList,
		colorAttachment->GetTarget(),
		EImageLayout::ColorAttachmentOptimal);

	const auto depthAttachmentLayout = RHI::IsDepthStencilFormat(depthAttachment->GetFormat()) ?
		EImageLayout::DepthStencilAttachmentOptimal :
		EImageLayout::DepthAttachmentOptimal;
	commands->ImageMemoryBarrier(commandList,
		depthAttachment,
		depthAttachmentLayout);

	commands->BeginRenderPass(commandList,
		TVector<RHI::RHISurfacePtr>{ colorAttachment },
		depthAttachment,
		glm::vec4(0,
			0,
			colorAttachment->GetTarget()->GetExtent().x,
			colorAttachment->GetTarget()->GetExtent().y),
		glm::ivec2(0, 0),
		false,
		glm::vec4(0.0f),
		0.0f,
		true);

#if defined(_WIN32)
	constexpr uint32_t MaxMeshesPerIndirectBatch = 16384u;
#else
	constexpr uint32_t MaxMeshesPerIndirectBatch = 128u;
#endif

	uint32_t runBegin = 0;
	while (runBegin < m_orderedDrawItems.Num())
	{
		const OrderedDrawItem& firstItem = m_orderedDrawItems[runBegin];
		uint32_t runLimit = (std::max)(1u,
			(std::min)(MaxMeshesPerIndirectBatch,
				firstItem.m_batch.m_supportedMeshesPerBatch));
		uint32_t runEnd = runBegin + 1u;
		while (runEnd < m_orderedDrawItems.Num())
		{
			const OrderedDrawItem& nextItem = m_orderedDrawItems[runEnd];
			if (!(firstItem.m_batch == nextItem.m_batch))
			{
				break;
			}

			const uint32_t nextLimit = (std::max)(1u,
				(std::min)(MaxMeshesPerIndirectBatch,
					nextItem.m_batch.m_supportedMeshesPerBatch));
			runLimit = (std::min)(runLimit, nextLimit);
			if (runEnd - runBegin >= runLimit)
			{
				break;
			}

			runEnd++;
		}

		const RHIBatch& batch = firstItem.m_batch;
		TVector<RHIShaderBindingSetPtr> shaderBindings({
			sceneView.m_frameBindings,
			sceneView.m_rhiLightsData,
			m_perInstanceData,
			batch.m_material->GetBindings(),
			batch.m_textureBindings });
		if (sceneView.m_boneMatrices)
		{
			shaderBindings.Add(sceneView.m_boneMatrices);
		}

		commands->BindMaterial(commandList, batch.m_material);
		commands->SetViewport(commandList,
			(float)viewport.x,
			(float)viewport.y,
			(float)viewport.z,
			(float)viewport.w,
			glm::vec2(scissor.x, scissor.y),
			glm::vec2(scissor.z, scissor.w),
			0.0f,
			1.0f);
		commands->BindShaderBindings(commandList,
			batch.m_material,
			shaderBindings);
		commands->BindVertexBuffer(commandList,
			batch.m_mesh->m_vertexBuffer,
			0);
		commands->BindIndexBuffer(commandList,
			batch.m_mesh->m_indexBuffer,
			0);

		const uint32_t runSize = runEnd - runBegin;
		commands->DrawIndexedIndirect(commandList,
			m_indirectBuffers[0],
			sizeof(RHI::DrawIndexedIndirectData) * runBegin,
			runSize,
			sizeof(RHI::DrawIndexedIndirectData));
		m_drawCallStats.m_numBatches++;
		m_drawCallStats.m_numInstances += runSize;

		runBegin = runEnd;
	}

	commands->EndRenderPass(commandList);
	commands->EndDebugRegion(commandList);
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

	RHI::RHITexturePtr transmissionFramebuffer =
		GetResolvedAttachment("transmissionFramebuffer");
	if (transmissionFramebuffer)
	{
		auto rhiLightsData = sceneView.m_rhiLightsData;
		constexpr const char* transmissionSamplerName =
			"g_transmissionFramebufferSampler";
		RHI::RHIShaderBindingPtr& transmissionBinding =
			rhiLightsData->GetOrAddShaderBinding(
				transmissionSamplerName);
		if (transmissionBinding->GetTextureBinding() !=
			transmissionFramebuffer)
		{
			driver->AddSamplerToShaderBindings(
				rhiLightsData,
				transmissionSamplerName,
				transmissionFramebuffer,
				10);
			rhiLightsData->RecalculateCompatibility();
		}
	}

	if (m_numMeshes == 0)
	{
		m_syncSharedResources.Unlock();
		return;
	}

	if (transmissionFramebuffer)
	{
		commands->ImageMemoryBarrier(
			commandList,
			transmissionFramebuffer,
			RHI::EImageLayout::ShaderReadOnlyOptimal);
	}

	if (!m_perInstanceData || m_sizePerInstanceData < sizeof(RenderSceneNode::PerInstanceData) * m_numMeshes)
	{
		SAILOR_PROFILE_SCOPE("Create storage for matrices");

		m_perInstanceData = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
		Sailor::RHI::Renderer::GetDriver()->AddSsboToShaderBindings(m_perInstanceData, "data", sizeof(RenderSceneNode::PerInstanceData), m_numMeshes, 0);
		m_sizePerInstanceData = sizeof(RenderSceneNode::PerInstanceData) * m_numMeshes;
	}

	RHI::RHIShaderBindingPtr storageBinding = m_perInstanceData->GetOrAddShaderBinding("data");
	if (GetSortingOrder() == RHI::ESortingOrder::BackToFront)
	{
		ProcessBackToFront(frameGraph,
			transferCommandList,
			commandList,
			sceneView,
			storageBinding,
			QueueTag);
		m_syncSharedResources.Unlock();
		return;
	}

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
		SpinLock transferCommandListLock;

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
							&transferCommandListLock);
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
							&m_cullingIndirectBufferBinding[i + 1],
							&transferCommandListLock);
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
					&transferCommandListLock);
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
					&m_cullingIndirectBufferBinding[0],
					&transferCommandListLock);
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
	m_orderedDrawItems.Clear();
	m_indirectBuffers.Clear();
	m_perInstanceData.Clear();
#if defined(__APPLE__)
	m_textureBindingCache.Clear();
#endif
}
