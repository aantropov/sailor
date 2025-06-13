#include "VulkanGraphicsDriver.h"
#include "RHI/Texture.h"
#include "RHI/RenderTarget.h"
#include "RHI/Cubemap.h"
#include "RHI/Surface.h"
#include "RHI/Fence.h"
#include "RHI/Mesh.h"
#include "RHI/Buffer.h"
#include "RHI/Material.h"
#include "RHI/Shader.h"
#include "RHI/VertexDescription.h"
#include "RHI/CommandList.h"
#include "RHI/Types.h"
#include "Memory.h"
#include "Memory/MemoryBlockAllocator.hpp"
#include "Platform/Win32/Window.h"
#include "VulkanApi.h"
#include "VulkanImageView.h"
#include "VulkanImage.h"
#include "VulkanCommandBuffer.h"
#include "VulkanPipeline.h"
#include "VulkanPipileneStates.h"
#include "VulkanShaderModule.h"
#include "VulkanBufferMemory.h"
#include "VulkanPipeline.h"
#include "VulkanDescriptors.h"
#include "RHI/Shader.h"
#include "Submodules/RenderDocApi.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "AssetRegistry/AssetRegistry.h"
#include "RHI/Renderer.h"

using namespace Sailor;
using namespace Sailor::GraphicsDriver::Vulkan;

void VulkanGraphicsDriver::Initialize(Win32::Window* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug)
{
	GraphicsDriver::Vulkan::VulkanApi::Initialize(pViewport, msaaSamples, bIsDebug);
	m_vkInstance = GraphicsDriver::Vulkan::VulkanApi::GetInstance();

	m_backBuffer = RHI::RHIRenderTargetPtr::Make(RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Repeat, false, RHI::EImageLayout::PresentSrc);
	m_depthStencilBuffer = RHI::RHIRenderTargetPtr::Make(RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Repeat, false, RHI::EImageLayout::DepthStencilAttachmentOptimal);

	if (RenderDocApi* renderDocApi = App::GetSubmodule<RenderDocApi>())
	{
		renderDocApi->SetActiveWindow((*((void**)(m_vkInstance->GetVkInstance()))), pViewport->GetHWND());
	}

	DWORD invalidColor = 0xe567ff;
	auto defaultImage = VulkanApi::CreateImage_Immediate(m_vkInstance->GetMainDevice(), &invalidColor, sizeof(DWORD), VkExtent3D{ 1,1,1 }, 1,
		VK_IMAGE_TYPE_2D,
		VkFormat::VK_FORMAT_R8G8B8A8_SRGB,
		VK_IMAGE_TILING_OPTIMAL,
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		VK_SHARING_MODE_EXCLUSIVE,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	auto defaultCubemap = VulkanApi::CreateImage_Immediate(m_vkInstance->GetMainDevice(), &invalidColor, sizeof(DWORD), VkExtent3D{ 1,1,1 }, 1,
		VK_IMAGE_TYPE_2D,
		VkFormat::VK_FORMAT_R8G8B8A8_SRGB,
		VK_IMAGE_TILING_OPTIMAL,
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		VK_SHARING_MODE_EXCLUSIVE,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
		6);

	m_defaultTexture = RHI::RHITexturePtr::Make(RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Repeat, false, RHI::EImageLayout::PresentSrc);

	m_vkDefaultTexture = VulkanApi::CreateImageView(m_vkInstance->GetMainDevice(), defaultImage, VkImageAspectFlagBits::VK_IMAGE_ASPECT_COLOR_BIT);
	m_vkDefaultCubemap = VulkanApi::CreateImageView(m_vkInstance->GetMainDevice(), defaultCubemap, VkImageAspectFlagBits::VK_IMAGE_ASPECT_COLOR_BIT);

	m_defaultTexture->m_vulkan.m_image = defaultImage;
	m_defaultTexture->m_vulkan.m_imageView = m_vkDefaultTexture;
}

VulkanGraphicsDriver::~VulkanGraphicsDriver()
{
	m_materialSsboAllocator.Clear();
	m_generalSsboAllocator.Clear();
	m_meshSsboAllocator.Clear();

	m_vkInstance->GetMainDevice()->Shutdown();

	// Waiting finishing releasing of rendering resources
	App::GetSubmodule<Tasks::Scheduler>()->WaitIdle(
		{
			EThreadType::Render,
			EThreadType::RHI,
			EThreadType::Main,
			EThreadType::Worker
		});

	check(m_cachedDescriptorSets.Num() == 0);

	GraphicsDriver::Vulkan::VulkanApi::Shutdown();
}

void VulkanGraphicsDriver::BeginConditionalDestroy()
{
	m_cachedMsaaRenderTargets.Clear();
	m_temporaryRenderTargets.Clear();
	m_cachedComputePipelines.Clear();
	m_cachedDescriptorSets.Clear();
	m_backBuffer.Clear();
	m_depthStencilBuffer.Clear();
	m_defaultTexture.Clear();
	m_vkDefaultTexture.Clear();
	m_vkDefaultCubemap.Clear();

	TrackResources_ThreadSafe();

	m_trackedFences.Clear();
	m_uniformBuffers.Clear();

	m_vkInstance->GetMainDevice()->BeginConditionalDestroy();
}

uint32_t VulkanGraphicsDriver::GetNumSubmittedCommandBuffers() const
{
	return m_vkInstance->GetMainDevice()->GetNumSubmittedCommandBufers();
}

bool VulkanGraphicsDriver::ShouldFixLostDevice(const Win32::Window* pViewport)
{
	return m_vkInstance->GetMainDevice()->ShouldFixLostDevice(pViewport);
}

bool VulkanGraphicsDriver::FixLostDevice(Win32::Window* pViewport)
{
	SAILOR_PROFILE_FUNCTION();

	if (m_vkInstance->GetMainDevice()->ShouldFixLostDevice(pViewport))
	{
		auto fixLostDevice_RenderThread = [this, pViewport = pViewport]() mutable
			{
				SAILOR_PROFILE_SCOPE("Fix lost device");
				m_vkInstance->WaitIdle();
				m_vkInstance->GetMainDevice()->FixLostDevice(pViewport);
				m_cachedMsaaRenderTargets.Clear();
				m_temporaryRenderTargets.Clear();
			};

		auto task = Tasks::CreateTask("Fix lost device", fixLostDevice_RenderThread, EThreadType::Render);
		task->Run();
		task->Wait();

		return true;
	}

	return false;
}

TVector<bool> VulkanGraphicsDriver::IsCompatible(VulkanPipelineLayoutPtr layout, const TVector<RHI::RHIShaderBindingSetPtr>& bindings) const
{
	SAILOR_PROFILE_FUNCTION();

	TVector<VulkanDescriptorSetPtr> sets;
	for (auto& binding : bindings)
	{
		sets.Add(binding->m_vulkan.m_descriptorSet);
	}

	return VulkanApi::IsCompatible(layout, sets);
}

RHI::RHIRenderTargetPtr VulkanGraphicsDriver::GetBackBuffer() const
{
	return m_backBuffer;
}

RHI::RHIRenderTargetPtr VulkanGraphicsDriver::GetDepthBuffer() const
{
	return m_depthStencilBuffer;
}

bool VulkanGraphicsDriver::AcquireNextImage()
{
	SAILOR_PROFILE_FUNCTION();

	const bool bRes = m_vkInstance->GetMainDevice()->AcquireNextImage();

	m_backBuffer->m_vulkan.m_imageView = m_vkInstance->GetMainDevice()->GetBackBuffer();
	m_backBuffer->m_vulkan.m_image = m_backBuffer->m_vulkan.m_imageView->m_image;

	if (m_depthStencilBuffer->m_vulkan.m_imageView != m_vkInstance->GetMainDevice()->GetDepthBuffer())
	{
		m_depthStencilBuffer->m_vulkan.m_imageView = m_vkInstance->GetMainDevice()->GetDepthBuffer();
		m_depthStencilBuffer->m_vulkan.m_image = m_depthStencilBuffer->m_vulkan.m_imageView->m_image;

		if (RHI::IsDepthFormat(m_depthStencilBuffer->GetFormat()))
		{
			m_depthStencilBuffer->m_depthAspect = RHI::RHITexturePtr::Make(m_depthStencilBuffer->GetFiltration(), m_depthStencilBuffer->GetClamping(), false, m_depthStencilBuffer->GetDefaultLayout());
			m_depthStencilBuffer->m_depthAspect->m_vulkan.m_image = m_depthStencilBuffer->m_vulkan.m_image;
			m_depthStencilBuffer->m_depthAspect->m_vulkan.m_imageView = VulkanImageViewPtr::Make(m_vkInstance->GetMainDevice(), m_depthStencilBuffer->m_depthAspect->m_vulkan.m_image, VK_IMAGE_ASPECT_DEPTH_BIT);
			m_depthStencilBuffer->m_depthAspect->m_vulkan.m_imageView->Compile();
		}

		if (RHI::IsDepthStencilFormat(m_depthStencilBuffer->GetFormat()))
		{
			m_depthStencilBuffer->m_stencilAspect = RHI::RHITexturePtr::Make(m_depthStencilBuffer->GetFiltration(), m_depthStencilBuffer->GetClamping(), false, m_depthStencilBuffer->GetDefaultLayout());
			m_depthStencilBuffer->m_stencilAspect->m_vulkan.m_image = m_depthStencilBuffer->m_vulkan.m_image;
			m_depthStencilBuffer->m_stencilAspect->m_vulkan.m_imageView = VulkanImageViewPtr::Make(m_vkInstance->GetMainDevice(), m_depthStencilBuffer->m_stencilAspect->m_vulkan.m_image, VK_IMAGE_ASPECT_STENCIL_BIT);
			m_depthStencilBuffer->m_stencilAspect->m_vulkan.m_imageView->Compile();
		}
	}

	return bRes;
}

bool VulkanGraphicsDriver::PresentFrame(const class FrameState& state,
	const TVector<RHI::RHICommandListPtr>& primaryCommandBuffers,
	const TVector<RHI::RHISemaphorePtr>& waitSemaphores) const
{
	SAILOR_PROFILE_FUNCTION();

	const TVector<VulkanCommandBufferPtr> primaryBuffers = primaryCommandBuffers.Select<VulkanCommandBufferPtr>([](const auto& lhs) { return lhs->m_vulkan.m_commandBuffer; });
	const TVector<VulkanSemaphorePtr> vkWaitSemaphores = waitSemaphores.Select<VulkanSemaphorePtr>([](const auto& lhs) { return lhs->m_vulkan.m_semaphore; });

	return m_vkInstance->GetMainDevice()->PresentFrame(state, primaryBuffers, vkWaitSemaphores);
}

void VulkanGraphicsDriver::WaitIdle()
{
	SAILOR_PROFILE_FUNCTION();

	m_vkInstance->WaitIdle();
}

void VulkanGraphicsDriver::SubmitCommandList(RHI::RHICommandListPtr commandList, RHI::RHIFencePtr fence, RHI::RHISemaphorePtr signalSemaphore, RHI::RHISemaphorePtr waitSemaphore)
{
	SAILOR_PROFILE_FUNCTION();

	//if we have fence and that is null we should create device resource
	if (fence && !fence->m_vulkan.m_fence)
	{
		SAILOR_PROFILE_SCOPE("Create fence");
		fence->m_vulkan.m_fence = VulkanFencePtr::Make(m_vkInstance->GetMainDevice());
	}

	TVector<VulkanSemaphorePtr> signal;
	TVector<VulkanSemaphorePtr> wait;

	{
		SAILOR_PROFILE_SCOPE("Add semaphore dependencies");

		if (signalSemaphore)
		{
			commandList->m_vulkan.m_commandBuffer->AddDependency(signalSemaphore->m_vulkan.m_semaphore);
			signal.Add(signalSemaphore->m_vulkan.m_semaphore);
		}

		if (waitSemaphore)
		{
			commandList->m_vulkan.m_commandBuffer->AddDependency(waitSemaphore->m_vulkan.m_semaphore);
			wait.Add(waitSemaphore->m_vulkan.m_semaphore);
		}
	}

	m_vkInstance->GetMainDevice()->SubmitCommandBuffer(commandList->m_vulkan.m_commandBuffer, fence ? fence->m_vulkan.m_fence : nullptr, signal, wait);

	if (fence)
	{
		SAILOR_PROFILE_SCOPE("Add fence dependencies");

		// Fence should hold command list during execution
		fence->AddDependency(commandList);

		// We should remove fence after execution
		TrackPendingCommandList_ThreadSafe(fence);
	}
}

RHI::RHISemaphorePtr VulkanGraphicsDriver::CreateWaitSemaphore()
{
	SAILOR_PROFILE_FUNCTION();
	auto device = m_vkInstance->GetMainDevice();

	RHI::RHISemaphorePtr res = RHI::RHISemaphorePtr::Make();
	res->m_vulkan.m_semaphore = VulkanSemaphorePtr::Make(device);
	return res;
}

RHI::RHICommandListPtr VulkanGraphicsDriver::CreateCommandList(bool bIsSecondary, RHI::ECommandListQueue queue)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	RHI::RHICommandListPtr cmdList = RHI::RHICommandListPtr::Make(queue);
	VkCommandBufferLevel level = bIsSecondary ? VK_COMMAND_BUFFER_LEVEL_SECONDARY : VK_COMMAND_BUFFER_LEVEL_PRIMARY;

	switch (queue)
	{
	case RHI::ECommandListQueue::Graphics:
		cmdList->m_vulkan.m_commandBuffer = VulkanCommandBufferPtr::Make(device, device->GetCurrentThreadContext().m_commandPool, level);
		break;
	case RHI::ECommandListQueue::Transfer:
		cmdList->m_vulkan.m_commandBuffer = VulkanCommandBufferPtr::Make(device, device->GetCurrentThreadContext().m_transferCommandPool, level);
		break;
	case RHI::ECommandListQueue::Compute:
		cmdList->m_vulkan.m_commandBuffer = VulkanCommandBufferPtr::Make(device, device->GetCurrentThreadContext().m_computeCommandPool, level);
		break;
	}

	return cmdList;
}

RHI::RHIBufferPtr VulkanGraphicsDriver::CreateIndirectBuffer(size_t size)
{
	SAILOR_PROFILE_FUNCTION();

	const uint32_t usage = RHI::EBufferUsageBit::IndirectBuffer_Bit | RHI::EBufferUsageBit::StorageBuffer_Bit | RHI::EBufferUsageBit::BufferTransferDst_Bit | RHI::EBufferUsageBit::BufferTransferSrc_Bit;
	const RHI::EMemoryPropertyFlags properties = RHI::EMemoryPropertyBit::HostCoherent | RHI::EMemoryPropertyBit::HostVisible;

	RHI::RHIBufferPtr res = RHI::RHIBufferPtr::Make(usage, properties);
	auto buffer = m_vkInstance->CreateBuffer(m_vkInstance->GetMainDevice(), size, (uint16_t)usage, (VkMemoryPropertyFlagBits)properties);

	// Hack to store ordinary buffer in TMemoryPtr
	res->m_vulkan.m_buffer = TMemoryPtr<VulkanBufferMemoryPtr>(0, 0, buffer->m_size, VulkanBufferMemoryPtr(buffer, 0, buffer->m_size), -1);

	return res;
}

RHI::RHIBufferPtr VulkanGraphicsDriver::CreateBuffer(size_t size, RHI::EBufferUsageFlags usage, RHI::EMemoryPropertyFlags properties)
{
	SAILOR_PROFILE_FUNCTION();

	RHI::RHIBufferPtr res = RHI::RHIBufferPtr::Make(usage, properties);
	auto buffer = m_vkInstance->CreateBuffer(m_vkInstance->GetMainDevice(), size, (uint16_t)usage, properties);

	// Hack to store ordinary buffer in TMemoryPtr
	res->m_vulkan.m_buffer = TMemoryPtr<VulkanBufferMemoryPtr>(0, 0, buffer->m_size, VulkanBufferMemoryPtr(buffer, 0, buffer->m_size), -1);

	return res;
}

RHI::RHIBufferPtr VulkanGraphicsDriver::CreateBuffer(RHI::RHICommandListPtr& cmdList, const void* pData, size_t size, RHI::EBufferUsageFlags usage, RHI::EMemoryPropertyFlags properties)
{
	SAILOR_PROFILE_FUNCTION();

	RHI::RHIBufferPtr outBuffer = CreateBuffer(size, usage, properties);

	auto buffer = m_vkInstance->CreateBuffer(cmdList->m_vulkan.m_commandBuffer,
		m_vkInstance->GetMainDevice(),
		pData, size, (uint16_t)usage);

	// Hack to store ordinary buffer in TMemoryPtr
	outBuffer->m_vulkan.m_buffer = TMemoryPtr<VulkanBufferMemoryPtr>(0, 0, buffer->m_size, VulkanBufferMemoryPtr(buffer, 0, buffer->m_size), -1);

	return outBuffer;
}

RHI::RHIShaderPtr VulkanGraphicsDriver::CreateShader(RHI::EShaderStage shaderStage, const RHI::ShaderByteCode& shaderSpirv)
{
	SAILOR_PROFILE_FUNCTION();

	auto res = RHI::RHIShaderPtr::Make(shaderStage);
	res->m_vulkan.m_shader = VulkanShaderStagePtr::Make((VkShaderStageFlagBits)shaderStage, "main", m_vkInstance->GetMainDevice(), shaderSpirv);
	res->m_vulkan.m_shader->Compile();

	return res;
}

RHI::RHIBufferPtr VulkanGraphicsDriver::CreateBuffer_Immediate(const void* pData, size_t size, RHI::EBufferUsageFlags usage)
{
	SAILOR_PROFILE_FUNCTION();

	RHI::RHIBufferPtr res = RHI::RHIBufferPtr::Make(usage, RHI::EMemoryPropertyBit::DeviceLocal);
	auto buffer = m_vkInstance->CreateBuffer_Immediate(m_vkInstance->GetMainDevice(), pData, size, (uint32_t)usage);

	// Hack to store ordinary buffer in TMemoryPtr
	res->m_vulkan.m_buffer = TMemoryPtr<VulkanBufferMemoryPtr>(0, 0, buffer->m_size, VulkanBufferMemoryPtr(buffer, 0, buffer->m_size), -1);
	return res;
}

void VulkanGraphicsDriver::CopyBufferToImage(RHI::RHICommandListPtr cmd, RHI::RHIBufferPtr src, RHI::RHITexturePtr dst)
{
	cmd->m_vulkan.m_commandBuffer->CopyBufferToImage(*src->m_vulkan.m_buffer, dst->m_vulkan.m_image, dst->GetExtent().x, dst->GetExtent().y, 1, 0u);
}

void VulkanGraphicsDriver::CopyImageToBuffer(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr src, RHI::RHIBufferPtr dst)
{
	cmd->m_vulkan.m_commandBuffer->CopyImageToBuffer(*dst->m_vulkan.m_buffer, src->m_vulkan.m_image, src->GetExtent().x, src->GetExtent().y, 1, 0u);
}

void VulkanGraphicsDriver::CopyBuffer_Immediate(RHI::RHIBufferPtr src, RHI::RHIBufferPtr dst, size_t size)
{
	m_vkInstance->CopyBuffer_Immediate(m_vkInstance->GetMainDevice(), *src->m_vulkan.m_buffer, *dst->m_vulkan.m_buffer, size);
}

void VulkanGraphicsDriver::RestoreImageBarriers(RHI::RHICommandListPtr cmd)
{
	for (const auto& barrier : cmd->m_vulkan.m_commandBuffer->GetImageBarriers())
	{
		RHI::RHITexturePtr tex = (*barrier.Second()).First();

		ImageMemoryBarrier(cmd, tex, tex->GetDefaultLayout());
	}

	cmd->m_vulkan.m_commandBuffer->GetImageBarriers().Clear();
}

void VulkanGraphicsDriver::SetDebugName(RHI::RHIResourcePtr resource, const std::string& name)
{
#ifdef _DEBUG
	auto device = m_vkInstance->GetMainDevice();

	if (auto texture = resource.DynamicCast<RHI::RHICubemap>())
	{
		device->SetDebugName(VkObjectType::VK_OBJECT_TYPE_IMAGE,
			(uint64_t)(VkImage)*texture->m_vulkan.m_image, "Cubemap " + name);
	}
	else if (auto texture = resource.DynamicCast<RHI::RHITexture>())
	{
		device->SetDebugName(VkObjectType::VK_OBJECT_TYPE_IMAGE,
			(uint64_t)(VkImage)*texture->m_vulkan.m_image, "Image " + name);
	}
	else if (auto material = resource.DynamicCast<RHI::RHIMaterial>())
	{
		for (const auto& pipeline : material->m_vulkan.m_pipelines)
		{
			device->SetDebugName(VkObjectType::VK_OBJECT_TYPE_PIPELINE,
				(uint64_t)(VkPipeline)*pipeline, "Pipeline " + name);
		}
	}
	else if (auto shader = resource.DynamicCast<RHI::RHIShader>())
	{
		device->SetDebugName(VkObjectType::VK_OBJECT_TYPE_SHADER_MODULE,
			(uint64_t)(VkShaderModule)*shader->m_vulkan.m_shader->m_module, "Shader Module " + name);
	}
	else if (auto cmdList = resource.DynamicCast<RHI::RHICommandList>())
	{
		device->SetDebugName(VkObjectType::VK_OBJECT_TYPE_COMMAND_BUFFER,
			(uint64_t)(VkCommandBuffer)*cmdList->m_vulkan.m_commandBuffer,
			"Command List " + name);
	}
	else if (auto sampler = resource.DynamicCast<VulkanSampler>())
	{
		device->SetDebugName(VkObjectType::VK_OBJECT_TYPE_COMMAND_BUFFER,
			(uint64_t)(VkSampler)*sampler,
			"Sampler " + name);
	}
	else if (auto semaphore = resource.DynamicCast<RHI::RHISemaphore>())
	{
		device->SetDebugName(VkObjectType::VK_OBJECT_TYPE_SEMAPHORE,
			(uint64_t)(VkSemaphore) * (semaphore->m_vulkan.m_semaphore),
			"Semaphore " + name);
	}
	else if (auto fence = resource.DynamicCast<RHI::RHIFence>())
	{
		if (fence->m_vulkan.m_fence)
		{
			device->SetDebugName(VkObjectType::VK_OBJECT_TYPE_FENCE,
				(uint64_t)(VkFence) * (fence->m_vulkan.m_fence),
				"Fence " + name);
		}
	}
#endif
}

void VulkanGraphicsDriver::BeginDebugRegion(RHI::RHICommandListPtr cmdList, const std::string& title, const glm::vec4& color)
{
#ifdef _DEBUG
	// We don't support debug regions for transfer only lists due to the limitations of the queue
	if (!cmdList->IsTransferOnly())
	{
		auto device = m_vkInstance->GetMainDevice();

		VkDebugMarkerMarkerInfoEXT markerInfo = {};
		markerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_MARKER_MARKER_INFO_EXT;
		memcpy(markerInfo.color, &color, sizeof(glm::vec4));
		markerInfo.pMarkerName = title.c_str();

		device->vkCmdDebugMarkerBegin(cmdList->m_vulkan.m_commandBuffer, &markerInfo);
	}
#endif
}

void VulkanGraphicsDriver::EndDebugRegion(RHI::RHICommandListPtr cmdList)
{
#ifdef _DEBUG
	// We don't support debug regions for transfer only lists due to the limitations of the queue
	if (!cmdList->IsTransferOnly())
	{
		auto device = m_vkInstance->GetMainDevice();
		device->vkCmdDebugMarkerEnd(cmdList->m_vulkan.m_commandBuffer);
	}
#endif
}

RHI::RHITexturePtr VulkanGraphicsDriver::CreateImage_Immediate(
	const void* pData,
	size_t size,
	glm::ivec3 extent,
	uint32_t mipLevels,
	RHI::ETextureType type,
	RHI::ETextureFormat format,
	RHI::ETextureFiltration filtration,
	RHI::ETextureClamping clamping,
	RHI::ETextureUsageFlags usage)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	VkExtent3D vkExtent;
	vkExtent.width = extent.x;
	vkExtent.height = extent.y;
	vkExtent.depth = extent.z;

	uint32_t flags = 0;
	uint32_t arrayLayers = 1;

	if (type == RHI::ETextureType::Cubemap)
	{
		type = RHI::ETextureType::Texture2D;
		flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
		arrayLayers = 6;
	}

	RHI::RHITexturePtr res = RHI::RHITexturePtr::Make(filtration, clamping, mipLevels > 1, RHI::EImageLayout::ShaderReadOnlyOptimal);
	res->m_vulkan.m_image = m_vkInstance->CreateImage_Immediate(m_vkInstance->GetMainDevice(),
		pData,
		size,
		vkExtent,
		mipLevels,
		(VkImageType)type,
		(VkFormat)format,
		VK_IMAGE_TILING_OPTIMAL,
		(uint32_t)usage,
		VkSharingMode::VK_SHARING_MODE_EXCLUSIVE,
		(VkImageLayout)RHI::EImageLayout::ShaderReadOnlyOptimal,
		flags,
		arrayLayers);

	res->m_vulkan.m_imageView = VulkanImageViewPtr::Make(device, res->m_vulkan.m_image);
	res->m_vulkan.m_imageView->Compile();

	return res;
}

void* VulkanGraphicsDriver::ExportImage(RHI::RHITexturePtr image)
{
	if (!image || !image->m_vulkan.m_image)
	{
		return nullptr;
	}

	auto device = m_vkInstance->GetMainDevice();
	return VulkanApi::ExportImage(device, image->m_vulkan.m_image);
}

RHI::RHITexturePtr VulkanGraphicsDriver::ImportImage(void* handle,
	glm::ivec3 extent,
	RHI::ETextureFormat format,
	RHI::ETextureUsageFlags usage,
	RHI::EImageLayout layout)
{
	auto device = m_vkInstance->GetMainDevice();

	VkExtent3D vkExtent{ (uint32_t)extent.x, (uint32_t)extent.y, (uint32_t)extent.z };
	auto vkImage = VulkanApi::ImportImage(device, handle, vkExtent, (VkFormat)format,
		(VkImageUsageFlags)usage, (VkImageLayout)layout);

	RHI::RHITexturePtr result = RHI::RHITexturePtr::Make(RHI::ETextureFiltration::Linear,
		RHI::ETextureClamping::Clamp, false, layout);
	result->m_vulkan.m_image = vkImage;
	result->m_vulkan.m_imageView = VulkanImageViewPtr::Make(device, vkImage);
	result->m_vulkan.m_imageView->Compile();

	return result;
}

RHI::RHITexturePtr VulkanGraphicsDriver::CreateTexture(
	const void* pData,
	size_t size,
	glm::ivec3 extent,
	uint32_t mipLevels,
	RHI::ETextureType type,
	RHI::ETextureFormat format,
	RHI::ETextureFiltration filtration,
	RHI::ETextureClamping clamping,
	RHI::ETextureUsageFlags usage,
	RHI::ESamplerReductionMode reduction)
{
	SAILOR_PROFILE_FUNCTION();

	// For textures we always use shader read only as default
	const RHI::EImageLayout layout = RHI::EImageLayout::ShaderReadOnlyOptimal;

	auto device = m_vkInstance->GetMainDevice();
	RHI::RHITexturePtr outTexture = RHI::RHITexturePtr::Make(filtration, clamping, mipLevels > 1, layout);

	VkExtent3D vkExtent;
	vkExtent.width = extent.x;
	vkExtent.height = extent.y;
	vkExtent.depth = extent.z;

	RHI::RHICommandListPtr cmdList = RHI::Renderer::GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Graphics);
	RHI::Renderer::GetDriver()->SetDebugName(cmdList, "Create Texture");
	RHI::Renderer::GetDriverCommands()->BeginCommandList(cmdList, true);

	uint32_t flags = 0;
	uint32_t arrayLayers = 1;

	if (type == RHI::ETextureType::Cubemap)
	{
		type = RHI::ETextureType::Texture2D;
		flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
		arrayLayers = 6;
	}

	outTexture->m_vulkan.m_image = m_vkInstance->CreateImage(cmdList->m_vulkan.m_commandBuffer,
		m_vkInstance->GetMainDevice(),
		pData,
		size,
		vkExtent,
		mipLevels,
		(VkImageType)type,
		(VkFormat)format,
		VK_IMAGE_TILING_OPTIMAL,
		(uint32_t)usage,
		VkSharingMode::VK_SHARING_MODE_EXCLUSIVE,
		(VkImageLayout)layout,
		flags,
		arrayLayers);

	RHI::Renderer::GetDriverCommands()->EndCommandList(cmdList);

	outTexture->m_vulkan.m_imageView = VulkanImageViewPtr::Make(device, outTexture->m_vulkan.m_image);
	outTexture->m_vulkan.m_imageView->Compile();
	outTexture->m_vulkan.m_image->m_defaultLayout = (VkImageLayout)(outTexture->GetDefaultLayout());

	RHI::RHIFencePtr fenceUpdateRes = RHI::RHIFencePtr::Make();
	RHI::Renderer::GetDriver()->SetDebugName(fenceUpdateRes, "Create Texture");

	TrackDelayedInitialization(outTexture.GetRawPtr(), fenceUpdateRes);
	SubmitCommandList(cmdList, fenceUpdateRes);

	return outTexture;
}

SAILOR_API RHI::RHICubemapPtr VulkanGraphicsDriver::CreateCubemap(
	glm::ivec2 extent,
	uint32_t mipLevels,
	RHI::ETextureFormat format,
	RHI::ETextureFiltration filtration,
	RHI::ETextureClamping clamping,
	RHI::ETextureUsageFlags usage,
	RHI::ESamplerReductionMode reduction)
{
	SAILOR_PROFILE_FUNCTION();

	VkImageLayout layout = (VkImageLayout)RHI::EImageLayout::ShaderReadOnlyOptimal;
	VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;

	if (const bool bIsUsedAsStorage = usage & VK_IMAGE_USAGE_STORAGE_BIT)
	{
		layout = (VkImageLayout)RHI::EImageLayout::General;
		if (RHI::IsDepthFormat(format))
		{
			tiling = VK_IMAGE_TILING_LINEAR;
		}
	}

	auto device = m_vkInstance->GetMainDevice();
	RHI::RHICubemapPtr outCubemap = RHI::RHICubemapPtr::Make(filtration, clamping, mipLevels > 1, (RHI::EImageLayout)layout, reduction);

	VkExtent3D vkExtent;
	vkExtent.width = extent.x;
	vkExtent.height = extent.y;
	vkExtent.depth = 1;

	outCubemap->m_vulkan.m_image = m_vkInstance->CreateImage(m_vkInstance->GetMainDevice(),
		vkExtent,
		mipLevels,
		VkImageType::VK_IMAGE_TYPE_2D,
		(VkFormat)format,
		tiling,
		(uint32_t)usage,
		VkSharingMode::VK_SHARING_MODE_EXCLUSIVE,
		VkSampleCountFlagBits::VK_SAMPLE_COUNT_1_BIT,
		layout,
		(VkImageCreateFlags)VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
		6);

	for (uint32_t i = 0; i < mipLevels; i++)
	{
		RHI::RHICubemapPtr res = outCubemap;

		if (i > 0)
		{
			res = RHI::RHICubemapPtr::Make(filtration, clamping, false, (RHI::EImageLayout)layout, reduction);
			res->m_vulkan.m_image = outCubemap->m_vulkan.m_image;
			res->m_vulkan.m_imageView = VulkanImageViewPtr::Make(outCubemap->m_vulkan.m_image->GetDevice(), outCubemap->m_vulkan.m_image);
			res->m_vulkan.m_imageView->m_subresourceRange.baseMipLevel = i;
			res->m_vulkan.m_imageView->m_subresourceRange.levelCount = 1;
			res->m_vulkan.m_imageView->Compile();

			outCubemap->m_mipLevels.Add(res);
		}

		for (uint32_t face = 0; face < 6; face++)
		{
			RHI::RHITexturePtr target = RHI::RHITexturePtr::Make(filtration, clamping, false, (RHI::EImageLayout)layout, reduction);

			target->m_vulkan.m_image = outCubemap->m_vulkan.m_image;
			target->m_vulkan.m_imageView = VulkanImageViewPtr::Make(device, target->m_vulkan.m_image);
			target->m_vulkan.m_imageView->m_viewType = VK_IMAGE_VIEW_TYPE_2D;
			target->m_vulkan.m_imageView->m_subresourceRange.baseMipLevel = i;
			target->m_vulkan.m_imageView->m_subresourceRange.levelCount = 1;
			target->m_vulkan.m_imageView->m_subresourceRange.baseArrayLayer = face;
			target->m_vulkan.m_imageView->m_subresourceRange.layerCount = 1;
			target->m_vulkan.m_imageView->Compile();

			res->m_faces.Emplace(target);
		}
	}

	outCubemap->m_vulkan.m_imageView = VulkanImageViewPtr::Make(device, outCubemap->m_vulkan.m_image);
	outCubemap->m_vulkan.m_imageView->Compile();
	outCubemap->m_vulkan.m_image->m_defaultLayout = (VkImageLayout)(outCubemap->GetDefaultLayout());

	RHI::RHICommandListPtr cmdList = RHI::Renderer::GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Graphics);
	RHI::Renderer::GetDriver()->SetDebugName(cmdList, "Create Cubemap");
	RHI::Renderer::GetDriverCommands()->BeginCommandList(cmdList, true);

	RHI::Renderer::GetDriverCommands()->ImageMemoryBarrier(cmdList,
		outCubemap,
		outCubemap->GetFormat(),
		RHI::EImageLayout::Undefined,
		(RHI::EImageLayout)layout);

	RHI::Renderer::GetDriverCommands()->EndCommandList(cmdList);

	RHI::RHIFencePtr fenceUpdateRes = RHI::RHIFencePtr::Make();
	RHI::Renderer::GetDriver()->SetDebugName(fenceUpdateRes, "Create Cubemap");

	TrackDelayedInitialization(outCubemap.GetRawPtr(), fenceUpdateRes);
	SubmitCommandList(cmdList, fenceUpdateRes);

	return outCubemap;
}

RHI::RHIRenderTargetPtr VulkanGraphicsDriver::CreateRenderTarget(
	glm::ivec2 extent,
	uint32_t mipLevels,
	RHI::ETextureFormat format,
	RHI::ETextureFiltration filtration,
	RHI::ETextureClamping clamping,
	RHI::ETextureUsageFlags usage,
	RHI::ESamplerReductionMode reduction)
{
	// Update layout
	RHI::RHICommandListPtr cmdList = RHI::Renderer::GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Graphics);
	RHI::Renderer::GetDriver()->SetDebugName(cmdList, "Create Render Target");
	RHI::Renderer::GetDriverCommands()->BeginCommandList(cmdList, true);

	RHI::RHIRenderTargetPtr outTexture = CreateRenderTarget(
		cmdList,
		extent,
		mipLevels,
		format,
		filtration,
		clamping,
		usage,
		reduction);

	RHI::Renderer::GetDriverCommands()->EndCommandList(cmdList);

	RHI::RHIFencePtr fenceUpdateRes = RHI::RHIFencePtr::Make();
	RHI::Renderer::GetDriver()->SetDebugName(fenceUpdateRes, "Create Render Target");

	TrackDelayedInitialization(outTexture.GetRawPtr(), fenceUpdateRes);
	SubmitCommandList(cmdList, fenceUpdateRes);

	return outTexture;
}

RHI::RHIRenderTargetPtr VulkanGraphicsDriver::CreateRenderTarget(
	RHI::RHICommandListPtr cmdList,
	glm::ivec2 extent,
	uint32_t mipLevels,
	RHI::ETextureFormat format,
	RHI::ETextureFiltration filtration,
	RHI::ETextureClamping clamping,
	RHI::ETextureUsageFlags usage,
	RHI::ESamplerReductionMode reduction)
{
	SAILOR_PROFILE_FUNCTION();

	VkImageLayout layout = (VkImageLayout)RHI::EImageLayout::ShaderReadOnlyOptimal;
	VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;

	if (const bool bIsUsedAsStorage = usage & VK_IMAGE_USAGE_STORAGE_BIT)
	{
		layout = (VkImageLayout)RHI::EImageLayout::General;

		if (RHI::IsDepthFormat(format))
		{
			tiling = VK_IMAGE_TILING_LINEAR;
		}
	}
	else if (const bool bIsUsedAsColorAttachment = usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
	{
		layout = (VkImageLayout)RHI::EImageLayout::ColorAttachmentOptimal;
	}
	else if (const bool bIsUsedAsDepthAttachment = usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
	{
		layout = (VkImageLayout)RHI::EImageLayout::DepthStencilAttachmentOptimal;
	}

	// Not optimal code
	//check(layout != (VkImageLayout)RHI::EImageLayout::General);

	auto device = m_vkInstance->GetMainDevice();
	RHI::RHIRenderTargetPtr outTexture = RHI::RHIRenderTargetPtr::Make(filtration, clamping, mipLevels > 1, (RHI::EImageLayout)layout, reduction);

	VkExtent3D vkExtent;
	vkExtent.width = extent.x;
	vkExtent.height = extent.y;
	vkExtent.depth = 1;

	outTexture->m_vulkan.m_image = m_vkInstance->CreateImage(m_vkInstance->GetMainDevice(),
		vkExtent,
		mipLevels,
		VkImageType::VK_IMAGE_TYPE_2D,
		(VkFormat)format,
		tiling,
		(uint32_t)usage,
		VkSharingMode::VK_SHARING_MODE_EXCLUSIVE,
		VkSampleCountFlagBits::VK_SAMPLE_COUNT_1_BIT,
		layout);

	outTexture->m_vulkan.m_image->m_defaultLayout = (VkImageLayout)(outTexture->GetDefaultLayout());
	outTexture->m_vulkan.m_imageView = VulkanImageViewPtr::Make(device, outTexture->m_vulkan.m_image);
	outTexture->m_vulkan.m_imageView->Compile();

	if (RHI::IsDepthFormat(format))
	{
		outTexture->m_depthAspect = RHI::RHITexturePtr::Make(filtration, clamping, false, (RHI::EImageLayout)layout, reduction);
		outTexture->m_depthAspect->m_vulkan.m_image = outTexture->m_vulkan.m_image;
		outTexture->m_depthAspect->m_vulkan.m_imageView = VulkanImageViewPtr::Make(device, outTexture->m_depthAspect->m_vulkan.m_image, VK_IMAGE_ASPECT_DEPTH_BIT);
		outTexture->m_depthAspect->m_vulkan.m_imageView->Compile();

		if (RHI::IsDepthStencilFormat(format))
		{
			outTexture->m_stencilAspect = RHI::RHITexturePtr::Make(filtration, clamping, false, (RHI::EImageLayout)layout, reduction);
			outTexture->m_stencilAspect->m_vulkan.m_image = outTexture->m_vulkan.m_image;
			outTexture->m_stencilAspect->m_vulkan.m_imageView = VulkanImageViewPtr::Make(device, outTexture->m_stencilAspect->m_vulkan.m_image, VK_IMAGE_ASPECT_STENCIL_BIT);
			outTexture->m_stencilAspect->m_vulkan.m_imageView->Compile();
		}
	}

	for (uint32_t i = 0; i < mipLevels; i++)
	{
		// TODO: Should we support ranges for mips?
		RHI::RHITexturePtr mipLevel = RHI::RHITexturePtr::Make(filtration, clamping, false, (RHI::EImageLayout)layout, reduction);

		mipLevel->m_vulkan.m_image = outTexture->m_vulkan.m_image;
		mipLevel->m_vulkan.m_imageView = VulkanImageViewPtr::Make(device, mipLevel->m_vulkan.m_image);
		mipLevel->m_vulkan.m_imageView->m_subresourceRange.baseMipLevel = i;
		mipLevel->m_vulkan.m_imageView->m_subresourceRange.levelCount = 1;
		mipLevel->m_vulkan.m_imageView->Compile();

		outTexture->m_mipLayers.Emplace(mipLevel);
	}

	RHI::Renderer::GetDriverCommands()->ImageMemoryBarrier(cmdList,
		outTexture,
		outTexture->GetFormat(),
		RHI::EImageLayout::Undefined,
		(RHI::EImageLayout)layout);

	return outTexture;
}

RHI::RHISurfacePtr VulkanGraphicsDriver::CreateSurface(
	glm::ivec2 extent,
	uint32_t mipLevels,
	RHI::ETextureFormat format,
	RHI::ETextureFiltration filtration,
	RHI::ETextureClamping clamping,
	RHI::ETextureUsageFlags usage)
{
	SAILOR_PROFILE_FUNCTION();

	RHI::RHISurfacePtr res;

	auto device = m_vkInstance->GetMainDevice();

	const RHI::RHIRenderTargetPtr resolved = CreateRenderTarget(extent, mipLevels, format, filtration, clamping, usage);
	RHI::RHIRenderTargetPtr target = resolved;

	bool bNeedsResolved = m_vkInstance->GetMainDevice()->GetCurrentMsaaSamples() != VK_SAMPLE_COUNT_1_BIT;
	if (bNeedsResolved)
	{
		target = RHI::RHIRenderTargetPtr::Make(filtration, clamping, false, RHI::EImageLayout::ColorAttachmentOptimal);

		VkExtent3D vkExtent;
		vkExtent.width = extent.x;
		vkExtent.height = extent.y;
		vkExtent.depth = 1;

		// Disable storage for MSAA target
		usage = usage & ~RHI::ETextureUsageBit::Storage_Bit;
		//usage = usage & ~VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

		target->m_vulkan.m_image = m_vkInstance->CreateImage(m_vkInstance->GetMainDevice(),
			vkExtent,
			1, // MSAA Don't support mips
			VkImageType::VK_IMAGE_TYPE_2D,
			(VkFormat)format,
			VK_IMAGE_TILING_OPTIMAL,
			(uint32_t)usage,
			(VkSharingMode)VkSharingMode::VK_SHARING_MODE_EXCLUSIVE,
			device->GetCurrentMsaaSamples(),
			(VkImageLayout)RHI::EImageLayout::ColorAttachmentOptimal);

		target->m_vulkan.m_image->m_defaultLayout = (VkImageLayout)(target->GetDefaultLayout());

		target->m_vulkan.m_imageView = VulkanImageViewPtr::Make(device, target->m_vulkan.m_image);
		target->m_vulkan.m_imageView->Compile();

		RHI::RHICommandListPtr cmdList = RHI::Renderer::GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Graphics);
		RHI::Renderer::GetDriver()->SetDebugName(cmdList, "Create Surface");
		RHI::Renderer::GetDriverCommands()->BeginCommandList(cmdList, true);
		RHI::Renderer::GetDriverCommands()->ImageMemoryBarrier(cmdList,
			target,
			target->GetFormat(),
			RHI::EImageLayout::Undefined,
			RHI::EImageLayout::ColorAttachmentOptimal);
		RHI::Renderer::GetDriverCommands()->EndCommandList(cmdList);

		RHI::RHIFencePtr fenceUpdateRes = RHI::RHIFencePtr::Make();
		RHI::Renderer::GetDriver()->SetDebugName(fenceUpdateRes, "Create Surface");

		TrackDelayedInitialization(target.GetRawPtr(), fenceUpdateRes);
		SubmitCommandList(cmdList, fenceUpdateRes);
	}

	return  RHI::RHISurfacePtr::Make(target, resolved, bNeedsResolved);
}

void VulkanGraphicsDriver::UpdateDescriptorSet(RHI::RHIShaderBindingSetPtr bindings)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();
	TVector<VulkanDescriptorPtr> descriptors;
	TVector<VkDescriptorSetLayoutBinding> descriptionSetLayouts;

	for (const auto& binding : bindings->GetShaderBindings())
	{
		if (binding.m_second->IsBind())
		{
			if (binding.m_second->GetTextureBindings().Num() > 0)
			{
				uint32_t index = 0;
				for (auto& texture : binding.m_second->GetTextureBindings())
				{
					if (binding.m_second->GetLayout().m_type == RHI::EShaderBindingType::CombinedImageSampler)
					{
						auto descr = VulkanDescriptorCombinedImagePtr::Make(binding.m_second->m_vulkan.m_descriptorSetLayout.binding, index,
							device->GetSamplers()->GetSampler(texture->GetFiltration(), texture->GetClamping(), texture->HasMipMaps(), texture->GetSamplerReduction()),
							texture->m_vulkan.m_imageView);
						descriptors.Add(descr);
					}
					else if (binding.m_second->GetLayout().m_type == RHI::EShaderBindingType::StorageImage)
					{
						auto descr = VulkanDescriptorStorageImagePtr::Make(binding.m_second->m_vulkan.m_descriptorSetLayout.binding, index, texture->m_vulkan.m_imageView);
						descriptors.Add(descr);
					}
					index++;
				}

				descriptionSetLayouts.Add(binding.m_second->m_vulkan.m_descriptorSetLayout);
			}
			else if (binding.m_second->m_vulkan.m_valueBinding)
			{
				const auto type = binding.m_second->GetLayout().m_type;
				const bool bBindWithoutOffset = (type == RHI::EShaderBindingType::StorageBuffer && !binding.m_second->m_vulkan.m_bBindSsboWithOffset);

				auto& valueBinding = *(binding.m_second->m_vulkan.m_valueBinding->Get());
				auto descr = VulkanDescriptorBufferPtr::Make(binding.m_second->m_vulkan.m_descriptorSetLayout.binding, 0,
					valueBinding.m_buffer,
					bBindWithoutOffset ? 0 : valueBinding.m_offset,
					valueBinding.m_size,
					type);

				descriptors.Add(descr);
				descriptionSetLayouts.Add(binding.m_second->m_vulkan.m_descriptorSetLayout);
			}
		}
	}

	// Should we just update descriptor set instead of recreation?
	// VK_KHR_descriptor_update_template
	bindings->m_vulkan.m_descriptorSet = VulkanDescriptorSetPtr::Make(device,
		device->GetCurrentThreadContext().m_descriptorPool,
		VulkanDescriptorSetLayoutPtr::Make(device, descriptionSetLayouts),
		descriptors);

	bindings->m_vulkan.m_descriptorSet->Compile();

#ifndef _SHIPPING
	VkDescriptorSet handleSet = *bindings->m_vulkan.m_descriptorSet;
	static uint32_t s_debugIterator = 0;
	m_vkInstance->GetMainDevice()->SetDebugName(VkObjectType::VK_OBJECT_TYPE_DESCRIPTOR_SET, (uint64_t)handleSet, std::format("ShaderBinding's Descriptor Set {}", s_debugIterator++));
#endif 
}

RHI::RHIMaterialPtr VulkanGraphicsDriver::CreateMaterial(const RHI::RHIVertexDescriptionPtr& vertexDescription, RHI::EPrimitiveTopology topology, const RHI::RenderState& renderState, const Sailor::ShaderSetPtr& shader)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	TVector<VulkanDescriptorSetLayoutPtr> descriptorSetLayouts;
	TVector<RHI::ShaderLayoutBinding> bindings;

	// We need debug shaders to get full names from reflection
	const bool bHasValidShaders = shader->GetDebugVertexShaderRHI() && shader->GetDebugFragmentShaderRHI();
	if (!bHasValidShaders)
	{
		SAILOR_LOG_ERROR("Shader %s is not compiled or valid", shader->GetFileId().ToString().c_str());
		return nullptr;
	}

	VulkanApi::CreateDescriptorSetLayouts(device, { shader->GetDebugVertexShaderRHI()->m_vulkan.m_shader, shader->GetDebugFragmentShaderRHI()->m_vulkan.m_shader },
		descriptorSetLayouts, bindings);

	auto shaderBindings = CreateShaderBindings();

	{
		SAILOR_PROFILE_SCOPE("Create and update shader bindings");

		// TODO: move initialization to external code
		for (uint32_t i = 0; i < bindings.Num(); i++)
		{
			auto& layoutBinding = bindings[i];
			if (layoutBinding.m_set == 0 || layoutBinding.m_set == 1 || layoutBinding.m_set == 2)
			{
				// We skip 0 layout, it is frameData and would be binded in a different way
				// Also we skip 1 layout, that is per instance data and would be binded in a different way
				continue;
			}
			const auto& layoutBindings = descriptorSetLayouts[layoutBinding.m_set]->m_descriptorSetLayoutBindings;

			auto it = layoutBindings.FindIf([&layoutBinding](const auto& bind) { return bind.binding == layoutBinding.m_binding; });
			if (it == -1)
			{
				continue;
			}

			auto& vkLayoutBinding = layoutBindings[it];
			auto& binding = shaderBindings->GetOrAddShaderBinding(layoutBinding.m_name);

			if (layoutBinding.m_type == RHI::EShaderBindingType::UniformBuffer)
			{
				auto& uniformAllocator = GetUniformBufferAllocator(layoutBinding.m_name);
				binding->m_vulkan.m_valueBinding = TManagedMemoryPtr<VulkanBufferMemoryPtr, VulkanBufferAllocator>::Make(
					uniformAllocator->Allocate(layoutBinding.m_size, device->GetMinUboOffsetAlignment()),
					uniformAllocator);

				binding->m_vulkan.m_descriptorSetLayout = vkLayoutBinding;
				binding->SetLayout(layoutBinding);
			}
			else if (layoutBinding.m_type == RHI::EShaderBindingType::StorageBuffer)
			{
				auto& storageAllocator = GetMaterialSsboAllocator();

				// We're storing different material's data with its different size in one SSBO, we need to allign properly

				const uint32_t paddedSize = layoutBinding.m_paddedSize;
				binding->m_vulkan.m_valueBinding = TManagedMemoryPtr<VulkanBufferMemoryPtr, VulkanBufferAllocator>::Make(storageAllocator->Allocate(paddedSize, paddedSize), storageAllocator);
				binding->m_vulkan.m_descriptorSetLayout = vkLayoutBinding;

				check(((*(binding->m_vulkan.m_valueBinding->Get())).m_offset % paddedSize) == 0);
				uint32_t instanceIndex = (uint32_t)((*(binding->m_vulkan.m_valueBinding->Get())).m_offset / paddedSize);

				binding->m_vulkan.m_storageInstanceIndex = instanceIndex;
				binding->SetLayout(layoutBinding);
			}
		}
	}

	shaderBindings->SetLayoutShaderBindings(bindings);
	UpdateDescriptorSet(shaderBindings);

	return CreateMaterial(vertexDescription, topology, renderState, shader, shaderBindings);
}

bool VulkanGraphicsDriver::FillShadersLayout(RHI::RHIShaderBindingSetPtr& pShaderBindings, const TVector<RHI::RHIShaderPtr>& shaders, uint32_t setNum)
{
	auto device = m_vkInstance->GetMainDevice();

	TVector<VulkanDescriptorSetLayoutPtr> descriptorSetLayouts;
	TVector<RHI::ShaderLayoutBinding> bindings;

	TVector<VulkanShaderStagePtr> vulkanShaders;
	vulkanShaders.Reserve(shaders.Num());

	for (const auto& shader : shaders)
	{
		vulkanShaders.Add(shader->m_vulkan.m_shader);
	}

	// We need debug shaders to get full names from reflection
	if (VulkanApi::CreateDescriptorSetLayouts(device, vulkanShaders, descriptorSetLayouts, bindings))
	{
		bindings.RemoveAll([=](const auto& el) { return el.m_set != setNum; });
		pShaderBindings->SetLayoutShaderBindings(bindings);
		return true;
	}

	return false;
}

RHI::RHIMaterialPtr VulkanGraphicsDriver::CreateMaterial(const RHI::RHIVertexDescriptionPtr& vertexDescription, RHI::EPrimitiveTopology topology, const RHI::RenderState& renderState, const Sailor::ShaderSetPtr& shader, const RHI::RHIShaderBindingSetPtr& shaderBindigs)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	TVector<VulkanDescriptorSetLayoutPtr> descriptorSetLayouts;
	TVector<RHI::ShaderLayoutBinding> bindings;

	// We need debug shaders to get full names from reflection
	VulkanApi::CreateDescriptorSetLayouts(device, { shader->GetDebugVertexShaderRHI()->m_vulkan.m_shader, shader->GetDebugFragmentShaderRHI()->m_vulkan.m_shader },
		descriptorSetLayouts, bindings);

#ifdef _DEBUG
	const bool bIsDebug = true;
#else
	const bool bIsDebug = false;
#endif

	auto vertex = bIsDebug ? shader->GetDebugVertexShaderRHI() : shader->GetVertexShaderRHI();
	auto fragment = bIsDebug ? shader->GetDebugFragmentShaderRHI() : shader->GetFragmentShaderRHI();

	RHI::RHIMaterialPtr res = RHI::RHIMaterialPtr::Make(renderState, vertex, fragment);

	TVector<VkPushConstantRange> pushConstants;

	//for (const auto& pushConstant : shader->GetDebugVertexShaderRHI()->m_vulkan.m_shader->GetPushConstants())
	for (uint32_t i = 0; i < shader->GetDebugVertexShaderRHI()->m_vulkan.m_shader->GetPushConstants().Num(); i++)
	{
		VkPushConstantRange vkPushConstant;
		vkPushConstant.offset = 0;
		vkPushConstant.size = 256;
		vkPushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;

		pushConstants.Emplace(vkPushConstant);
	}

	// TODO: Rearrange descriptorSetLayouts to support vector of descriptor sets
	auto pipelineLayout = VulkanPipelineLayoutPtr::Make(device,
		descriptorSetLayouts,
		bindings,
		pushConstants,
		0);

	auto colorAttachments = TVector<VkFormat>(shader->GetColorAttachments());
	auto depthStencilFormat = (VkFormat)shader->GetDepthStencilAttachment();

	auto pipeline = VulkanGraphicsPipelinePtr::Make(device,
		pipelineLayout,
		TVector{ vertex->m_vulkan.m_shader, fragment->m_vulkan.m_shader },
		device->GetPipelineBuilder()->BuildPipeline(vertexDescription, vertex->m_vulkan.m_shader->GetVertexAttributesBindings(), topology, renderState, colorAttachments, depthStencilFormat),
		0);

	pipeline->m_renderPass = device->GetRenderPass();
	pipeline->Compile();

	res->m_vulkan.m_pipelines.Emplace(std::move(pipeline));

	res->SetBindings(shaderBindigs);

	return res;
}

TSharedPtr<VulkanBufferAllocator>& VulkanGraphicsDriver::GetMeshSsboAllocator()
{
	if (!m_meshSsboAllocator)
	{
		// 32 mb is a good point to start
		const size_t StorageBufferBlockSize = 32 * 1024 * 1024u;

		m_meshSsboAllocator = TSharedPtr<VulkanBufferAllocator>::Make(StorageBufferBlockSize, 1024 * 1024u, StorageBufferBlockSize);
		m_meshSsboAllocator->GetGlobalAllocator().SetUsage(RHI::EBufferUsageBit::StorageBuffer_Bit |
			RHI::EBufferUsageBit::VertexBuffer_Bit |
			RHI::EBufferUsageBit::IndexBuffer_Bit |
			VK_BUFFER_USAGE_TRANSFER_DST_BIT);

		m_meshSsboAllocator->GetGlobalAllocator().SetMemoryProperties(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	}
	return m_meshSsboAllocator;
}


TSharedPtr<VulkanBufferAllocator>& VulkanGraphicsDriver::GetGeneralSsboAllocator()
{
	if (!m_generalSsboAllocator)
	{
		// 1 mb should be starting point
		const size_t StorageBufferBlockSize = 1024 * 1024u;

		m_generalSsboAllocator = TSharedPtr<VulkanBufferAllocator>::Make(StorageBufferBlockSize, 1024 * 1024u, StorageBufferBlockSize);
		m_generalSsboAllocator->GetGlobalAllocator().SetUsage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
		m_generalSsboAllocator->GetGlobalAllocator().SetMemoryProperties(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	}
	return m_generalSsboAllocator;
}

TSharedPtr<VulkanBufferAllocator>& VulkanGraphicsDriver::GetMaterialSsboAllocator()
{
	if (!m_materialSsboAllocator)
	{
		// 8 mb should be enough to store all material data, 
		// pessimistically ~256b per material instance, ~32000 materials per world
		const size_t StorageBufferBlockSize = 8 * 1024 * 1024u;

		m_materialSsboAllocator = TSharedPtr<VulkanBufferAllocator>::Make(StorageBufferBlockSize, 1024 * 1024u, StorageBufferBlockSize);
		m_materialSsboAllocator->GetGlobalAllocator().SetUsage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
		m_materialSsboAllocator->GetGlobalAllocator().SetMemoryProperties(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	}
	return m_materialSsboAllocator;
}

TSharedPtr<VulkanBufferAllocator>& VulkanGraphicsDriver::GetUniformBufferAllocator(const std::string& uniformTypeId)
{
	SAILOR_PROFILE_FUNCTION();

	auto it = m_uniformBuffers.Find(uniformTypeId);
	if (it != m_uniformBuffers.end())
	{
		return (*it).m_second;
	}

	auto& uniformAllocator = m_uniformBuffers.At_Lock(uniformTypeId);

	uniformAllocator = TSharedPtr<VulkanBufferAllocator>::Make(1024 * 1024, 256, 1024 * 1024);
	uniformAllocator->GetGlobalAllocator().SetUsage(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	uniformAllocator->GetGlobalAllocator().SetMemoryProperties(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	m_uniformBuffers.Unlock(uniformTypeId);

	return uniformAllocator;
}

void VulkanGraphicsDriver::UpdateShaderBinding_Immediate(RHI::RHIShaderBindingSetPtr bindings, const std::string& parameter, const void* value, size_t size)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	RHI::RHICommandListPtr commandList = RHI::RHICommandListPtr::Make(RHI::ECommandListQueue::Transfer);
	commandList->m_vulkan.m_commandBuffer = Vulkan::VulkanCommandBufferPtr::Make(device, device->GetCurrentThreadContext().m_transferCommandPool, VkCommandBufferLevel::VK_COMMAND_BUFFER_LEVEL_SECONDARY);

	auto& shaderBinding = bindings->GetOrAddShaderBinding(parameter);

	// All uniform buffers should be bound
	check(shaderBinding->IsBind());

	UpdateShaderBinding(commandList, shaderBinding, value, size);

	SubmitCommandList_Immediate(commandList);
}

RHI::RHIShaderBindingSetPtr VulkanGraphicsDriver::CreateShaderBindings()
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	RHI::RHIShaderBindingSetPtr res = RHI::RHIShaderBindingSetPtr::Make();
	return res;
}

RHI::RHIShaderBindingPtr VulkanGraphicsDriver::AddShaderBinding(RHI::RHIShaderBindingSetPtr& pShaderBindings, const RHI::RHIShaderBindingPtr& binding, const std::string& name, uint32_t shaderBinding)
{
	auto& pBinding = pShaderBindings->GetOrAddShaderBinding(name);

	pBinding->m_vulkan = binding->m_vulkan;
	pBinding->m_vulkan.m_descriptorSetLayout = VulkanApi::CreateDescriptorSetLayoutBinding(shaderBinding, (VkDescriptorType)binding->GetLayout().m_type);

	RHI::ShaderLayoutBinding layout = binding->GetLayout();
	layout.m_binding = shaderBinding;
	layout.m_name = name;

	pBinding->SetLayout(layout);

	pShaderBindings->UpdateLayoutShaderBinding(layout);
	UpdateDescriptorSet(pShaderBindings);

	return pBinding;
}

// TODO: Refactoring remove code duplication
RHI::RHIShaderBindingPtr VulkanGraphicsDriver::AddBufferToShaderBindings(RHI::RHIShaderBindingSetPtr& pShaderBindings, RHI::RHIBufferPtr buffer, const std::string& name, uint32_t shaderBinding)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	RHI::RHIShaderBindingPtr binding = pShaderBindings->GetOrAddShaderBinding(name);

	auto vulkanBufferMemoryPtr = buffer->m_vulkan.m_buffer;
	binding->m_vulkan.m_valueBinding = TManagedMemoryPtr<VulkanBufferMemoryPtr, VulkanBufferAllocator>::Make(vulkanBufferMemoryPtr, TSharedPtr<VulkanBufferAllocator>(nullptr));
	binding->m_vulkan.m_storageInstanceIndex = 0;
	binding->m_vulkan.m_bBindSsboWithOffset = true;

	// // First try to find in existed layouts
	const auto& layouts = pShaderBindings->GetLayoutBindings();
	size_t index = layouts.FindIf([=](const auto& el) { return el.m_binding == shaderBinding; });

	auto bindingType = buffer->GetUsage() & RHI::EBufferUsageBit::StorageBuffer_Bit ? RHI::EShaderBindingType::StorageBuffer : RHI::EShaderBindingType::UniformBuffer;
	if (index == -1)
	{
		RHI::ShaderLayoutBinding layout;
		layout.m_binding = shaderBinding;
		layout.m_name = name;
		layout.m_size = (uint32_t)buffer->GetSize();
		layout.m_type = bindingType;
		layout.m_paddedSize = (uint32_t)buffer->GetSize();

		pShaderBindings->UpdateLayoutShaderBinding(layout);

		binding->SetLayout(layout);
	}
	else
	{
		binding->SetLayout(layouts[index]);
	}

	binding->m_vulkan.m_descriptorSetLayout = VulkanApi::CreateDescriptorSetLayoutBinding(shaderBinding, (VkDescriptorType)bindingType);
	UpdateDescriptorSet(pShaderBindings);

	return binding;
}

// TODO: Refactoring remove code duplication
RHI::RHIShaderBindingPtr VulkanGraphicsDriver::AddSsboToShaderBindings(RHI::RHIShaderBindingSetPtr& pShaderBindings, const std::string& name, size_t elementSize, size_t numElements, uint32_t shaderBinding, bool bBindSsboWithOffset)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	RHI::RHIShaderBindingPtr binding = pShaderBindings->GetOrAddShaderBinding(name);

	TSharedPtr<VulkanBufferAllocator> allocator = GetGeneralSsboAllocator();

	const size_t p = 16 - elementSize % 16;
	const size_t paddedSize = elementSize + p;

	size_t alignment = paddedSize;

	if (bBindSsboWithOffset)
	{
		// We should use the correct alignment
		const size_t reqAlignment = device->GetMinSsboOffsetAlignment();
		alignment = reqAlignment > paddedSize ? (reqAlignment % paddedSize == 0 ? reqAlignment : paddedSize * reqAlignment) : paddedSize;
	}

	auto vulkanBufferMemoryPtr = allocator->Allocate(paddedSize * numElements, alignment);
	binding->m_vulkan.m_valueBinding = TManagedMemoryPtr<VulkanBufferMemoryPtr, VulkanBufferAllocator>::Make(vulkanBufferMemoryPtr, allocator);

	check((*(binding->m_vulkan.m_valueBinding->Get())).m_offset % paddedSize == 0);

	binding->m_vulkan.m_storageInstanceIndex = bBindSsboWithOffset ? 0 : (uint32_t)((*(binding->m_vulkan.m_valueBinding->Get())).m_offset / paddedSize);
	binding->m_vulkan.m_bBindSsboWithOffset = bBindSsboWithOffset;

	// // First try to find in existed layouts
	const auto& layouts = pShaderBindings->GetLayoutBindings();
	size_t index = layouts.FindIf([=](const auto& el) { return el.m_binding == shaderBinding; });

	if (index == -1)
	{
		RHI::ShaderLayoutBinding layout;
		layout.m_binding = shaderBinding;
		layout.m_name = name;
		layout.m_size = (uint32_t)(numElements * elementSize);
		layout.m_type = RHI::EShaderBindingType::StorageBuffer;
		layout.m_paddedSize = (uint32_t)paddedSize;

		pShaderBindings->UpdateLayoutShaderBinding(layout);

		binding->SetLayout(layout);
	}
	else
	{
		binding->SetLayout(layouts[index]);
	}

	binding->m_vulkan.m_descriptorSetLayout = VulkanApi::CreateDescriptorSetLayoutBinding(shaderBinding, (VkDescriptorType)RHI::EShaderBindingType::StorageBuffer);
	UpdateDescriptorSet(pShaderBindings);

	return binding;
}

RHI::RHIShaderBindingPtr VulkanGraphicsDriver::AddBufferToShaderBindings(RHI::RHIShaderBindingSetPtr& pShaderBindings, const std::string& name, size_t size, uint32_t shaderBinding, RHI::EShaderBindingType bufferType)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	RHI::RHIShaderBindingPtr binding = pShaderBindings->GetOrAddShaderBinding(name);
	TSharedPtr<VulkanBufferAllocator> allocator;

	// TODO: rewrite strictly according to std430
	const size_t p = 16 - size % 16;
	const size_t paddedSize = size + p;

	if (const bool bIsStorage = bufferType == RHI::EShaderBindingType::StorageBuffer)
	{
		allocator = GetGeneralSsboAllocator();

		binding->m_vulkan.m_valueBinding = TManagedMemoryPtr<VulkanBufferMemoryPtr, VulkanBufferAllocator>::Make(allocator->Allocate(paddedSize, paddedSize), allocator);
		binding->m_vulkan.m_storageInstanceIndex = (uint32_t)((**binding->m_vulkan.m_valueBinding->Get()).m_offset / paddedSize);
	}
	else
	{
		allocator = GetUniformBufferAllocator(name);
		binding->m_vulkan.m_valueBinding = TManagedMemoryPtr<VulkanBufferMemoryPtr, VulkanBufferAllocator>::Make(allocator->Allocate(size, device->GetMinUboOffsetAlignment()), allocator);
	}

	// First try to find in existed layouts
	const auto& layouts = pShaderBindings->GetLayoutBindings();
	size_t index = layouts.FindIf([=](const auto& el) { return el.m_binding == shaderBinding; });

	if (index == -1)
	{
		RHI::ShaderLayoutBinding layout;
		layout.m_binding = shaderBinding;
		layout.m_name = name;
		layout.m_size = (uint32_t)size;
		layout.m_type = bufferType;
		layout.m_paddedSize = (uint32_t)paddedSize;

		pShaderBindings->UpdateLayoutShaderBinding(layout);

		binding->SetLayout(layout);
	}
	else
	{
		binding->SetLayout(layouts[index]);
	}

	binding->m_vulkan.m_descriptorSetLayout = VulkanApi::CreateDescriptorSetLayoutBinding(shaderBinding, (VkDescriptorType)bufferType);
	UpdateDescriptorSet(pShaderBindings);

	return binding;
}

RHI::RHIShaderBindingPtr VulkanGraphicsDriver::AddSamplerToShaderBindings(RHI::RHIShaderBindingSetPtr& pShaderBindings, const std::string& name, RHI::RHITexturePtr texture, uint32_t shaderBinding)
{
	return AddSamplerToShaderBindings(pShaderBindings, name, TVector<RHI::RHITexturePtr>{ texture }, shaderBinding);
}

RHI::RHIShaderBindingPtr VulkanGraphicsDriver::AddSamplerToShaderBindings(RHI::RHIShaderBindingSetPtr& pShaderBindings, const std::string& name, const TVector<RHI::RHITexturePtr>& array, uint32_t shaderBinding)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();
	RHI::RHIShaderBindingPtr binding = pShaderBindings->GetOrAddShaderBinding(name);

	RHI::ShaderLayoutBinding layout;
	layout.m_binding = shaderBinding;
	layout.m_name = name;
	layout.m_type = RHI::EShaderBindingType::CombinedImageSampler;
	layout.m_arrayCount = (uint32_t)array.Num();

	binding->SetLayout(layout);
	binding->m_vulkan.m_descriptorSetLayout = VulkanApi::CreateDescriptorSetLayoutBinding(layout.m_binding, (VkDescriptorType)layout.m_type, layout.m_arrayCount);
	binding->SetTextureBindings(array);

	pShaderBindings->UpdateLayoutShaderBinding(layout);
	UpdateDescriptorSet(pShaderBindings);

	return binding;
}

RHI::RHIShaderBindingPtr VulkanGraphicsDriver::AddStorageImageToShaderBindings(RHI::RHIShaderBindingSetPtr& pShaderBindings, const std::string& name, RHI::RHITexturePtr texture, uint32_t shaderBinding)
{
	return AddStorageImageToShaderBindings(pShaderBindings, name, TVector<RHI::RHITexturePtr>{ texture }, shaderBinding);
}

RHI::RHIShaderBindingPtr VulkanGraphicsDriver::AddStorageImageToShaderBindings(RHI::RHIShaderBindingSetPtr& pShaderBindings, const std::string& name, const TVector<RHI::RHITexturePtr>& array, uint32_t shaderBinding)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();
	RHI::RHIShaderBindingPtr binding = pShaderBindings->GetOrAddShaderBinding(name);

	RHI::ShaderLayoutBinding layout;
	layout.m_binding = shaderBinding;
	layout.m_name = name;
	layout.m_type = RHI::EShaderBindingType::StorageImage;
	layout.m_arrayCount = (uint32_t)array.Num();

	binding->SetLayout(layout);
	binding->m_vulkan.m_descriptorSetLayout = VulkanApi::CreateDescriptorSetLayoutBinding(layout.m_binding, (VkDescriptorType)layout.m_type, layout.m_arrayCount);
	binding->SetTextureBindings(array);

	pShaderBindings->UpdateLayoutShaderBinding(layout);
	UpdateDescriptorSet(pShaderBindings);

	return binding;
}

void VulkanGraphicsDriver::UpdateShaderBinding(RHI::RHIShaderBindingSetPtr bindings, const std::string& parameter, RHI::RHITexturePtr value, uint32_t dstArrayElement)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();
	const auto& layoutBindings = bindings->GetLayoutBindings();

	auto index = layoutBindings.FindIf([&parameter](const RHI::ShaderLayoutBinding& shaderLayoutBinding)
		{
			return shaderLayoutBinding.m_name == parameter;
		});

	if (index != -1)
	{
		auto textureBinding = bindings->GetOrAddShaderBinding(parameter);

		if (bindings->m_vulkan.m_descriptorSet != nullptr)
		{
			auto cmpFunc = [=](const VulkanDescriptorPtr& descriptor)
				{
					return descriptor->GetBinding() == layoutBindings[index].m_binding && descriptor->GetArrayElement() == dstArrayElement;
				};

			auto& descriptors = bindings->m_vulkan.m_descriptorSet->m_descriptors;

			uint32_t arrayIndex = dstArrayElement;
			bool bFound = false;

			// Firstly we fast check by index, 95% that we hit
			if (dstArrayElement < descriptors.Num() && cmpFunc(descriptors[dstArrayElement]))
			{
				bFound = true;
			}
			else
			{
				auto descrIt = std::find_if(descriptors.begin(), descriptors.end(), cmpFunc);
				if (descrIt != descriptors.end())
				{
					arrayIndex = (uint32_t)(descrIt - descriptors.begin());
					bFound = true;
				}
			}

			if (bFound)
			{
				// Should we fully recreate descriptorSet to avoid race condition?
				textureBinding->SetTextureBinding(dstArrayElement, value);

				descriptors[arrayIndex] = VulkanDescriptorCombinedImagePtr::Make(layoutBindings[index].m_binding,
					dstArrayElement,
					device->GetSamplers()->GetSampler(value->GetFiltration(), value->GetClamping(), value->HasMipMaps(), value->GetSamplerReduction()),
					value->m_vulkan.m_imageView);

				bindings->m_vulkan.m_descriptorSet->UpdateDescriptor(arrayIndex);
				bindings->RecalculateCompatibility();
				return;
			}
		}

		// Add new texture binding
		check(dstArrayElement == 0);

		textureBinding->SetTextureBindings({ value });
		textureBinding->m_vulkan.m_descriptorSetLayout = VulkanApi::CreateDescriptorSetLayoutBinding(layoutBindings[index].m_binding, (VkDescriptorType)layoutBindings[index].m_type);
		textureBinding->SetLayout(layoutBindings[index]);
		UpdateDescriptorSet(bindings);

		return;
	}

	SAILOR_LOG("Trying to update not bound uniform sampler");
}

VulkanComputePipelinePtr VulkanGraphicsDriver::GetOrAddComputePipeline(RHI::RHIShaderPtr computeShader, uint32_t sizePushConstantsData)
{
	auto& computePipeline = m_cachedComputePipelines.At_Lock(computeShader);

	if (!computePipeline)
	{
		auto device = m_vkInstance->GetMainDevice();

		TVector<VulkanDescriptorSetLayoutPtr> descriptorSetLayouts;
		TVector<RHI::ShaderLayoutBinding> bindings;
		TVector<VkPushConstantRange> pushConstants;

		// We need debug shaders to get full names from reflection
		VulkanApi::CreateDescriptorSetLayouts(device, { computeShader->m_vulkan.m_shader }, descriptorSetLayouts, bindings);

		// We blindly believe the passed arguments
		check((sizePushConstantsData > 4) == (computeShader->m_vulkan.m_shader->GetPushConstants().Num() > 0));

		if (const bool bRequestPushConstants = (sizePushConstantsData > 4) || (computeShader->m_vulkan.m_shader->GetPushConstants().Num() > 0))
		{
			check(sizePushConstantsData > 4);

			VkPushConstantRange vkPushConstant;
			vkPushConstant.offset = 0;
			vkPushConstant.size = 256;
			vkPushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;

			pushConstants.Emplace(vkPushConstant);
		}

		auto pipelineLayout = VulkanPipelineLayoutPtr::Make(device, descriptorSetLayouts, bindings, pushConstants, 0);
		computePipeline = VulkanComputePipelinePtr::Make(device, pipelineLayout, computeShader->m_vulkan.m_shader);
		computePipeline->Compile();
	}

	m_cachedComputePipelines.Unlock(computeShader);

	return computePipeline;
}

RHI::RHITexturePtr VulkanGraphicsDriver::GetOrAddMsaaFramebufferRenderTarget(RHI::EFormat textureFormat, glm::ivec2 extent)
{
	size_t hash = (size_t)((size_t)textureFormat | (extent.x << 1) | (extent.y << 5));
	if (m_cachedMsaaRenderTargets.ContainsKey(hash))
	{
		return m_cachedMsaaRenderTargets[hash];
	}

	auto device = m_vkInstance->GetMainDevice();

	RHI::RHITexturePtr target = RHI::RHITexturePtr::Make(RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Clamp, false);

	VkExtent3D vkExtent;
	vkExtent.width = extent.x;
	vkExtent.height = extent.y;
	vkExtent.depth = 1;

	const bool bIsDepthFormat = textureFormat == RHI::EFormat::D16_UNORM || textureFormat == RHI::EFormat::D32_SFLOAT || textureFormat == RHI::EFormat::X8_D24_UNORM_PACK32 ||
		textureFormat == RHI::EFormat::D32_SFLOAT_S8_UINT;
	const auto usage = (uint32_t)(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		(!bIsDepthFormat ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));

	target->m_vulkan.m_image = m_vkInstance->CreateImage(m_vkInstance->GetMainDevice(),
		vkExtent,
		1,
		(VkImageType)RHI::ETextureType::Texture2D,
		(VkFormat)textureFormat,
		VK_IMAGE_TILING_OPTIMAL,
		usage,
		(VkSharingMode)VkSharingMode::VK_SHARING_MODE_EXCLUSIVE,
		device->GetCurrentMsaaSamples());

	target->m_vulkan.m_imageView = VulkanImageViewPtr::Make(device, target->m_vulkan.m_image);
	target->m_vulkan.m_imageView->Compile();

	m_cachedMsaaRenderTargets.At_Lock(hash) = target;
	m_cachedMsaaRenderTargets.Unlock(hash);

	return target;
}

void VulkanGraphicsDriver::PushConstants(RHI::RHICommandListPtr cmd, RHI::RHIMaterialPtr material, size_t size, const void* ptr)
{
	cmd->m_vulkan.m_commandBuffer->PushConstants(material->m_vulkan.m_pipelines[0]->m_layout, 0, size, ptr);
}

void VulkanGraphicsDriver::GenerateMipMaps(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr target)
{
	VkImage vkHandle = *target->m_vulkan.m_image;

	const bool bShouldOptimizeBarriers = cmd->m_vulkan.m_commandBuffer->GetImageBarriers().ContainsKey(vkHandle);
	if (bShouldOptimizeBarriers)
	{
		ImageMemoryBarrier(cmd, target, RHI::EImageLayout::TransferDstOptimal);
	}

	cmd->m_vulkan.m_commandBuffer->GenerateMipMaps(target->m_vulkan.m_image);

	if (bShouldOptimizeBarriers)
	{
		// Hack to fit the internal layout
		cmd->m_vulkan.m_commandBuffer->GetImageBarriers()[vkHandle] = TPair(target, (RHI::EImageLayout)target->m_vulkan.m_image->m_defaultLayout);
		//ImageMemoryBarrier(cmd, target, target->GetDefaultLayout());
	}
}

void VulkanGraphicsDriver::ConvertEquirect2Cubemap(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr equirect, RHI::RHICubemapPtr cubemap)
{
	if (!m_pEquirect2Cubemap)
	{
		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeEquirect2Cube.shader"))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderInfo->GetFileId(), m_pEquirect2Cubemap);
		}

		check(m_pEquirect2Cubemap);
	}

	// TODO: Should we cache the shader bindings?
	RHI::RHIShaderBindingSetPtr computeEquirect2Cubemap = CreateShaderBindings();

	AddSamplerToShaderBindings(computeEquirect2Cubemap, "src", equirect, 0);
	AddStorageImageToShaderBindings(computeEquirect2Cubemap, "dst", cubemap, 1);

	Dispatch(cmd, m_pEquirect2Cubemap->GetComputeShaderRHI(),
		(uint32_t)(equirect->GetExtent().x / 32.0f),
		(uint32_t)(equirect->GetExtent().y / 32.0f),
		6u,
		{ computeEquirect2Cubemap },
		nullptr, 0);
}

// IGraphicsDriverCommands

void VulkanGraphicsDriver::MemoryBarrier(RHI::RHICommandListPtr cmd, RHI::EAccessFlags srcAccess, RHI::EAccessFlags dstAccess)
{
	cmd->m_vulkan.m_commandBuffer->MemoryBarrier((VkAccessFlagBits)srcAccess, (VkAccessFlagBits)dstAccess);
}

void VulkanGraphicsDriver::ImageMemoryBarrier(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr image, RHI::EFormat format, RHI::EImageLayout layout, bool bAllowToWriteFromComputeShader)
{
	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = (VkImageLayout)layout;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = *image->m_vulkan.m_image;
	barrier.subresourceRange.aspectMask = VulkanApi::ComputeAspectFlagsForFormat((VkFormat)format);
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = image->m_vulkan.m_image->m_mipLevels;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = image->m_vulkan.m_image->m_arrayLayers;

	VkPipelineStageFlags sourceStage = bAllowToWriteFromComputeShader ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
	VkPipelineStageFlags destinationStage = bAllowToWriteFromComputeShader ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VulkanCommandBuffer::GetPipelineStage((VkImageLayout)layout);

	barrier.srcAccessMask = bAllowToWriteFromComputeShader ? VulkanCommandBuffer::GetAccessFlags((VkImageLayout)layout) : VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = bAllowToWriteFromComputeShader ? VK_ACCESS_SHADER_WRITE_BIT : VulkanCommandBuffer::GetAccessFlags((VkImageLayout)layout);

	vkCmdPipelineBarrier(
		*cmd->m_vulkan.m_commandBuffer,
		sourceStage, destinationStage,
		0,
		0, nullptr,
		0, nullptr,
		1, &barrier
	);

	cmd->m_vulkan.m_commandBuffer->AddDependency(image);
}

void VulkanGraphicsDriver::ImageMemoryBarrier(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr image, RHI::EImageLayout newLayout)
{
	if (m_bIsTrackingGpu)
	{
		m_lastFrameGpuStats.m_barriers[image][newLayout]++;
	}

	auto& imageBarriers = cmd->m_vulkan.m_commandBuffer->GetImageBarriers();

	VkImage vkHandle = *image->m_vulkan.m_image;
	if (!imageBarriers.ContainsKey(vkHandle))
	{
		imageBarriers[vkHandle] = TPair(image, image->GetDefaultLayout());
	}

	RHI::EImageLayout oldLayout = imageBarriers[vkHandle].Second();

	const bool bCompute = newLayout == RHI::EImageLayout::ComputeRead || newLayout == RHI::EImageLayout::ComputeWrite ||
		oldLayout == RHI::EImageLayout::ComputeRead || oldLayout == RHI::EImageLayout::ComputeWrite;

	if (!bCompute && (oldLayout == newLayout))
	{
		return;
	}

	ImageMemoryBarrier(cmd, image, image->GetFormat(), oldLayout, newLayout);

	imageBarriers[vkHandle] = TPair(image, newLayout);
}

void VulkanGraphicsDriver::ImageMemoryBarrier(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr image, RHI::EFormat format, RHI::EImageLayout oldLayout, RHI::EImageLayout newLayout)
{
	const VkImageLayout defaultComputeLayout = VkImageLayout::VK_IMAGE_LAYOUT_GENERAL;
	const VkAccessFlags oldComputeAccess = oldLayout == RHI::EImageLayout::ComputeWrite ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
	const VkAccessFlags newComputeAccess = newLayout == RHI::EImageLayout::ComputeWrite ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
	const VkPipelineStageFlagBits computeStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

	const bool bComputeOld = oldLayout == RHI::EImageLayout::ComputeWrite || oldLayout == RHI::EImageLayout::ComputeRead;
	const bool bComputeNew = newLayout == RHI::EImageLayout::ComputeWrite || newLayout == RHI::EImageLayout::ComputeRead;

	if (bComputeOld && bComputeNew)
	{
		cmd->m_vulkan.m_commandBuffer->ImageMemoryBarrier(image->m_vulkan.m_imageView,
			(VkFormat)format,
			defaultComputeLayout, defaultComputeLayout,
			oldComputeAccess, newComputeAccess,
			computeStage, computeStage);
	}
	else if (bComputeOld && !bComputeNew)
	{
		cmd->m_vulkan.m_commandBuffer->ImageMemoryBarrier(image->m_vulkan.m_imageView,
			(VkFormat)format,
			defaultComputeLayout, (VkImageLayout)newLayout,
			oldComputeAccess, VulkanCommandBuffer::GetAccessFlags((VkImageLayout)newLayout),
			computeStage, VulkanCommandBuffer::GetPipelineStage((VkImageLayout)newLayout));
	}
	else if (!bComputeOld && bComputeNew)
	{
		cmd->m_vulkan.m_commandBuffer->ImageMemoryBarrier(image->m_vulkan.m_imageView,
			(VkFormat)format,
			(VkImageLayout)oldLayout, defaultComputeLayout,
			VulkanCommandBuffer::GetAccessFlags((VkImageLayout)oldLayout), newComputeAccess,
			VulkanCommandBuffer::GetPipelineStage((VkImageLayout)oldLayout), computeStage);
	}
	else
	{
		cmd->m_vulkan.m_commandBuffer->ImageMemoryBarrier(image->m_vulkan.m_imageView, (VkFormat)format, (VkImageLayout)oldLayout, (VkImageLayout)newLayout);
	}
}

bool VulkanGraphicsDriver::BlitImage(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr src, RHI::RHITexturePtr dst, glm::ivec4 srcRegionRect, glm::ivec4 dstRegionRect, RHI::ETextureFiltration filtration)
{
	VkRect2D srcRect{};
	srcRect.offset.x = srcRegionRect.x;
	srcRect.offset.y = srcRegionRect.y;
	srcRect.extent.width = srcRegionRect.z;
	srcRect.extent.height = srcRegionRect.w;

	VkRect2D dstRect{};
	dstRect.offset.x = dstRegionRect.x;
	dstRect.offset.y = dstRegionRect.y;
	dstRect.extent.width = dstRegionRect.z;
	dstRect.extent.height = dstRegionRect.w;

	return cmd->m_vulkan.m_commandBuffer->BlitImage(src->m_vulkan.m_imageView, dst->m_vulkan.m_imageView, srcRect, dstRect, (VkFilter)filtration);
}

void VulkanGraphicsDriver::ClearDepthStencil(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr dst, float depth, uint32_t stencil)
{
	cmd->m_vulkan.m_commandBuffer->ClearDepthStencil(dst->m_vulkan.m_imageView, depth, stencil);
}

void VulkanGraphicsDriver::ClearImage(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr dst, const glm::vec4& clearColor)
{
	cmd->m_vulkan.m_commandBuffer->ClearImage(dst->m_vulkan.m_imageView, clearColor);
}

void VulkanGraphicsDriver::ExecuteSecondaryCommandList(RHI::RHICommandListPtr cmd, RHI::RHICommandListPtr cmdSecondary)
{
	//TODO: Check secondary
	cmd->m_vulkan.m_commandBuffer->Execute(cmdSecondary->m_vulkan.m_commandBuffer);
}

void VulkanGraphicsDriver::RenderSecondaryCommandBuffers(RHI::RHICommandListPtr cmd,
	TVector<RHI::RHICommandListPtr> secondaryCmds,
	const TVector<RHI::RHISurfacePtr>& colorAttachments,
	RHI::RHITexturePtr depthStencilAttachment,
	glm::ivec4 renderArea,
	glm::ivec2 offset,
	bool bClearRenderTargets,
	glm::vec4 clearColor,
	float clearDepth,
	bool bStoreDepth)
{
	const bool bNeedsResolve = (colorAttachments.Num() > 0 && colorAttachments[0]->NeedsResolve());
	if (!bNeedsResolve)
	{
		TVector<RHI::RHITexturePtr> resolved;
		resolved.Reserve(colorAttachments.Num());
		for (uint32_t i = 0; i < colorAttachments.Num(); i++)
		{
			resolved.Add(colorAttachments[i]->GetResolved());
		}

		RenderSecondaryCommandBuffers(cmd,
			secondaryCmds,
			resolved,
			depthStencilAttachment,
			renderArea,
			offset,
			bClearRenderTargets,
			clearColor,
			clearDepth,
			bStoreDepth);
	}
	else
	{
		TVector<VulkanImageViewPtr> targets(colorAttachments.Num());
		TVector<VulkanImageViewPtr> resolved(colorAttachments.Num());
		for (uint32_t i = 0; i < colorAttachments.Num(); i++)
		{
			targets[i] = colorAttachments[i]->GetTarget()->m_vulkan.m_imageView;
			resolved[i] = colorAttachments[i]->GetResolved()->m_vulkan.m_imageView;;
		}

		VkRect2D rect{};

		rect.extent.width = (uint32_t)renderArea.z;
		rect.extent.height = (uint32_t)renderArea.w;
		rect.offset.x = renderArea.x;
		rect.offset.y = renderArea.y;

		VkClearValue clearValue;
		clearValue.color = { {clearColor.x, clearColor.y, clearColor.z, clearColor.w } };
		clearValue.depthStencil = { clearDepth, 0 };// VulkanApi::DefaultClearDepthStencilValue;

		auto vulkanRenderer = App::GetSubmodule<RHI::Renderer>()->GetDriver().DynamicCast<VulkanGraphicsDriver>();
		VulkanImageViewPtr vulkanDepthStencil = depthStencilAttachment->m_vulkan.m_imageView;

		const auto depthExtents = glm::ivec2(vulkanDepthStencil->GetImage()->m_extent.width, vulkanDepthStencil->GetImage()->m_extent.height);
		VulkanImageViewPtr msaaDepthStencilTarget = vulkanRenderer->GetOrAddMsaaFramebufferRenderTarget((RHI::ETextureFormat)vulkanDepthStencil->GetImage()->m_format, depthExtents)->m_vulkan.m_imageView;

		cmd->m_vulkan.m_commandBuffer->BeginRenderPassEx(targets, resolved,
			msaaDepthStencilTarget,
			depthStencilAttachment->m_vulkan.m_imageView,
			rect,
			VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT_KHR,
			VkOffset2D{ .x = offset.x, .y = offset.y },
			bClearRenderTargets,
			clearValue,
			bStoreDepth);

		for (auto& el : secondaryCmds)
		{
			ExecuteSecondaryCommandList(cmd, el);
		}

		EndRenderPass(cmd);
	}
}

void VulkanGraphicsDriver::RenderSecondaryCommandBuffers(RHI::RHICommandListPtr cmd,
	TVector<RHI::RHICommandListPtr> secondaryCmds,
	const TVector<RHI::RHITexturePtr>& colorAttachments,
	RHI::RHITexturePtr depthStencilAttachment,
	glm::ivec4 renderArea,
	glm::ivec2 offset,
	bool bClearRenderTargets,
	glm::vec4 clearColor,
	float clearDepth,
	bool bSupportMultisampling,
	bool bStoreDepth)
{
	TVector<VulkanImageViewPtr> attachments(colorAttachments.Num());
	for (uint32_t i = 0; i < colorAttachments.Num(); i++)
	{
		attachments[i] = colorAttachments[i]->m_vulkan.m_imageView;
	}

	VkRect2D rect{};

	rect.extent.width = (uint32_t)renderArea.z;
	rect.extent.height = (uint32_t)renderArea.w;
	rect.offset.x = renderArea.x;
	rect.offset.y = renderArea.y;

	VkClearValue clearValue;
	clearValue.color = { {clearColor.x, clearColor.y, clearColor.z, clearColor.w} };
	clearValue.depthStencil = { clearDepth, 0 };// VulkanApi::DefaultClearDepthStencilValue;

	cmd->m_vulkan.m_commandBuffer->BeginRenderPassEx(attachments,
		depthStencilAttachment->m_vulkan.m_imageView,
		rect,
		VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT_KHR,
		VkOffset2D{ .x = offset.x, .y = offset.y },
		bSupportMultisampling,
		bClearRenderTargets,
		clearValue,
		bStoreDepth);

	for (auto& el : secondaryCmds)
	{
		ExecuteSecondaryCommandList(cmd, el);
	}

	EndRenderPass(cmd);
}

void VulkanGraphicsDriver::BeginRenderPass(RHI::RHICommandListPtr cmd,
	const TVector<RHI::RHITexturePtr>& colorAttachments,
	RHI::RHITexturePtr depthStencilAttachment,
	glm::ivec4 renderArea,
	glm::ivec2 offset,
	bool bClearRenderTargets,
	glm::vec4 clearColor,
	float clearDepth,
	bool bSupportMultisampling,
	bool bStoreDepth)
{
	TVector<VulkanImageViewPtr> attachments(colorAttachments.Num());
	for (uint32_t i = 0; i < colorAttachments.Num(); i++)
	{
		attachments[i] = colorAttachments[i]->m_vulkan.m_imageView;
	}

	VkRect2D rect{};

	rect.extent.width = (uint32_t)renderArea.z;
	rect.extent.height = (uint32_t)renderArea.w;
	rect.offset.x = renderArea.x;
	rect.offset.y = renderArea.y;

	VkClearValue clearValue;
	clearValue.color = { {clearColor.x, clearColor.y, clearColor.z, clearColor.w} };
	clearValue.depthStencil = { clearDepth, 0 };// VulkanApi::DefaultClearDepthStencilValue;

	cmd->m_vulkan.m_commandBuffer->BeginRenderPassEx(attachments,
		depthStencilAttachment ? depthStencilAttachment->m_vulkan.m_imageView : nullptr,
		rect,
		0,
		VkOffset2D{ .x = offset.x, .y = offset.y },
		bSupportMultisampling,
		bClearRenderTargets,
		clearValue,
		bStoreDepth);
}

void VulkanGraphicsDriver::BeginRenderPass(RHI::RHICommandListPtr cmd,
	const TVector<RHI::RHISurfacePtr>& colorAttachments,
	RHI::RHITexturePtr depthStencilAttachment,
	glm::ivec4 renderArea,
	glm::ivec2 offset,
	bool bClearRenderTargets,
	glm::vec4 clearColor,
	float clearDepth,
	bool bStoreDepth)
{
	const bool bNeedsResolve = (colorAttachments.Num() > 0 && colorAttachments[0]->NeedsResolve());
	if (!bNeedsResolve)
	{
		TVector<RHI::RHITexturePtr> resolved;
		resolved.Reserve(colorAttachments.Num());
		for (uint32_t i = 0; i < colorAttachments.Num(); i++)
		{
			resolved.Add(colorAttachments[i]->GetResolved());
		}

		BeginRenderPass(cmd, resolved, depthStencilAttachment,
			renderArea, offset, bClearRenderTargets, clearColor, clearDepth, false);
	}
	else
	{
		TVector<VulkanImageViewPtr> resolved;
		TVector<VulkanImageViewPtr> target;
		resolved.Reserve(colorAttachments.Num());
		target.Reserve(colorAttachments.Num());

		for (uint32_t i = 0; i < colorAttachments.Num(); i++)
		{
			resolved.Add(colorAttachments[i]->GetResolved()->m_vulkan.m_imageView);
			target.Add(colorAttachments[i]->GetTarget()->m_vulkan.m_imageView);
		}

		VkRect2D rect{};

		rect.extent.width = (uint32_t)renderArea.z;
		rect.extent.height = (uint32_t)renderArea.w;
		rect.offset.x = renderArea.x;
		rect.offset.y = renderArea.y;

		VkClearValue clearValue;
		clearValue.color = { {clearColor.x, clearColor.y, clearColor.z, clearColor.w} };
		clearValue.depthStencil = { clearDepth, 0 };// VulkanApi::DefaultClearDepthStencilValue;

		VulkanImageViewPtr msaaDepthStencilTarget{};
		VulkanImageViewPtr vulkanDepthStencil{};

		if (depthStencilAttachment)
		{
			auto vulkanRenderer = App::GetSubmodule<RHI::Renderer>()->GetDriver().DynamicCast<VulkanGraphicsDriver>();
			vulkanDepthStencil = depthStencilAttachment->m_vulkan.m_imageView;

			const auto depthExtents = glm::ivec2(vulkanDepthStencil->GetImage()->m_extent.width, vulkanDepthStencil->GetImage()->m_extent.height);
			msaaDepthStencilTarget = vulkanRenderer->GetOrAddMsaaFramebufferRenderTarget((RHI::ETextureFormat)vulkanDepthStencil->GetImage()->m_format, depthExtents)->m_vulkan.m_imageView;
		}

		cmd->m_vulkan.m_commandBuffer->BeginRenderPassEx(
			target,
			resolved,
			msaaDepthStencilTarget,
			vulkanDepthStencil,
			rect,
			0,
			VkOffset2D{ .x = offset.x, .y = offset.y },
			bClearRenderTargets,
			clearValue,
			bStoreDepth);
	}
}

void VulkanGraphicsDriver::EndRenderPass(RHI::RHICommandListPtr cmd)
{
	cmd->m_vulkan.m_commandBuffer->EndRenderPassEx();
}

void VulkanGraphicsDriver::BeginSecondaryCommandList(RHI::RHICommandListPtr cmd,
	bool bOneTimeSubmit,
	bool bSupportMultisampling,
	RHI::EFormat colorFormat)
{
	uint32_t flags = bOneTimeSubmit ? VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT : 0;

	check(cmd->m_vulkan.m_commandBuffer->GetLevel() == VK_COMMAND_BUFFER_LEVEL_SECONDARY);

	auto device = m_vkInstance->GetMainDevice();

	// This tells Vulkan that this secondary command buffer will be executed entirely inside a render pass.		
	flags |= VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;

	/*
	VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT specifies that a command buffer can be resubmitted to a queue while it is in the pending state,
	and recorded into multiple primary command buffers.

	TODO: That could affect the performance, so we don't want to use the bit
	*/

	if (!bOneTimeSubmit)
	{
		flags |= VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
	}

	cmd->m_vulkan.m_commandBuffer->BeginSecondaryCommandList(TVector<VkFormat>{(VkFormat)colorFormat}, device->GetDepthFormat(), flags, VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT, bSupportMultisampling);
}

void VulkanGraphicsDriver::BeginCommandList(RHI::RHICommandListPtr cmd, bool bOneTimeSubmit)
{
	check(cmd->m_vulkan.m_commandBuffer->GetLevel() != VK_COMMAND_BUFFER_LEVEL_SECONDARY);

	uint32_t flags = bOneTimeSubmit ? VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT : 0;
	cmd->m_vulkan.m_commandBuffer->BeginCommandList(flags);
}

void VulkanGraphicsDriver::EndCommandList(RHI::RHICommandListPtr cmd)
{
	RestoreImageBarriers(cmd);

	cmd->m_vulkan.m_commandBuffer->EndCommandList();
}

void VulkanGraphicsDriver::UpdateShaderBinding(RHI::RHICommandListPtr cmd, RHI::RHIShaderBindingPtr parameter, const void* pData, size_t size, size_t variableOffset)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	if (parameter->IsBind())
	{
		auto& binding = parameter->m_vulkan.m_valueBinding->Get();
		Update(cmd, *binding, pData, size, variableOffset);
	}
}

void VulkanGraphicsDriver::UpdateBuffer(RHI::RHICommandListPtr cmd, RHI::RHIBufferPtr buffer, const void* pData, size_t size, size_t offset)
{
	SAILOR_PROFILE_FUNCTION();

	if (buffer->GetUsage() & RHI::EBufferUsageBit::IndirectBuffer_Bit)
	{
		// We store IndirectBuffer in Device memory
		auto& vulkanBuffer = buffer->m_vulkan.m_buffer.m_ptr.m_buffer;
		vulkanBuffer->GetMemoryDevice()->Copy((*vulkanBuffer->GetBufferMemoryPtr()).m_offset + offset, size, pData);
	}
	else
	{
		Update(cmd, (*buffer->m_vulkan.m_buffer), pData, size, offset);
	}
}

void VulkanGraphicsDriver::Update(RHI::RHICommandListPtr cmd, VulkanBufferMemoryPtr bufferPtr, const void* data, size_t size, size_t offset)
{
	SAILOR_PROFILE_FUNCTION();
	auto dstBuffer = bufferPtr.m_buffer;
	auto device = m_vkInstance->GetMainDevice();

	const auto& requirements = dstBuffer->GetMemoryRequirements();

	VulkanBufferPtr stagingBuffer;
	size_t m_memoryOffset;

#if defined(SAILOR_VULKAN_COMBINE_STAGING_BUFFERS)

	TMemoryPtr<VulkanBufferMemoryPtr> stagingBufferManagedPtr;
	{
		SAILOR_PROFILE_SCOPE("Allocate space in staging buffer allocator");
		stagingBufferManagedPtr = device->GetStagingBufferAllocator()->Allocate(size, requirements.alignment);
	}

	m_memoryOffset = (**stagingBufferManagedPtr).m_offset;
	stagingBuffer = (*stagingBufferManagedPtr).m_buffer;
	cmd->m_vulkan.m_commandBuffer->AddDependency(stagingBufferManagedPtr, device->GetStagingBufferAllocator());
#elif defined(SAILOR_VULKAN_SHARE_DEVICE_MEMORY_FOR_STAGING_BUFFERS)

	auto& stagingMemoryAllocator = device->GetMemoryAllocator((VkMemoryPropertyFlags)(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT), requirements);

	SAILOR_PROFILE_BLOCK("Allocate staging memory"_h);
	auto pData = stagingMemoryAllocator.Allocate(size, requirements.alignment);
	SAILOR_PROFILE_END_BLOCK("Allocate staging memory"_h);

	SAILOR_PROFILE_BLOCK("Create staging buffer"_h);
	stagingBuffer = VulkanBufferPtr::Make(device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_CONCURRENT);
	stagingBuffer->Compile();
	VK_CHECK(stagingBuffer->Bind(pData));
	SAILOR_PROFILE_END_BLOCK("Create staging buffer"_h);

	m_memoryOffset = (*pData).m_offset;
#else

	SAILOR_PROFILE_BLOCK("Allocate staging device memory"_h);
	auto memReq = device->GetMemoryRequirements_StagingBuffer();
	memReq.size = std::max(memReq.size, size);

	auto deviceMemoryPtr = VulkanDeviceMemoryPtr::Make(device, memReq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	SAILOR_PROFILE_END_BLOCK("Allocate staging device memory"_h);

	SAILOR_PROFILE_BLOCK("Create staging buffer"_h);
	stagingBuffer = VulkanBufferPtr::Make(device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_CONCURRENT);
	stagingBuffer->Compile();
	VK_CHECK(stagingBuffer->Bind(deviceMemoryPtr, 0));
	SAILOR_PROFILE_END_BLOCK("Create staging buffer"_h);

	m_memoryOffset = 0;

	cmd->m_vulkan.m_commandBuffer->AddDependency(stagingBuffer);

	// stagingBuffer->GetBufferMemoryPtr()
#endif

	{
		SAILOR_PROFILE_SCOPE("Copy data to staging buffer");
		stagingBuffer->GetMemoryDevice()->Copy(m_memoryOffset, size, data);
	}

	{
		SAILOR_PROFILE_SCOPE("Copy from staging to video ram command");
		cmd->m_vulkan.m_commandBuffer->CopyBuffer(*stagingBufferManagedPtr, bufferPtr, size, 0, offset);
	}
}

void VulkanGraphicsDriver::SetMaterialParameter(RHI::RHICommandListPtr cmd, RHI::RHIShaderBindingSetPtr bindings, const std::string& binding, const std::string& variable, const void* value, size_t size)
{
	SAILOR_PROFILE_FUNCTION();

	if (bindings->HasBinding(binding))
	{
		auto device = m_vkInstance->GetMainDevice();
		auto& shaderBinding = bindings->GetOrAddShaderBinding(binding);
		UpdateShaderBindingVariable(cmd, shaderBinding, variable, value, size);
	}
}

void VulkanGraphicsDriver::UpdateShaderBindingVariable(RHI::RHICommandListPtr cmd, RHI::RHIShaderBindingPtr shaderBinding, const std::string& variable, const void* value, size_t size)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	// All uniform buffers should be bound
	check(shaderBinding->IsBind());

	RHI::ShaderLayoutBindingMember bindingLayout;
	if (!shaderBinding->FindVariableInUniformBuffer(variable, bindingLayout))
	{
		ensure(false, "Missing variable %s in shader", variable.c_str());
		return;
	}

	UpdateShaderBinding(cmd, shaderBinding, value, size, bindingLayout.m_absoluteOffset);
}

void VulkanGraphicsDriver::UpdateMesh(RHI::RHIMeshPtr mesh, const void* pVertices, size_t vertexBuffer, const void* pIndices, size_t indexBuffer)
{
	auto device = m_vkInstance->GetMainDevice();

	const VkDeviceSize bufferSize = vertexBuffer;
	const VkDeviceSize indexBufferSize = indexBuffer;

	RHI::RHICommandListPtr cmdList = CreateCommandList(false, RHI::ECommandListQueue::Transfer);
	RHI::Renderer::GetDriver()->SetDebugName(cmdList, "Update Mesh");
	RHI::Renderer::GetDriverCommands()->BeginCommandList(cmdList, true);

	const RHI::EBufferUsageFlags flags = RHI::EBufferUsageBit::VertexBuffer_Bit | RHI::EBufferUsageBit::IndexBuffer_Bit;
	const RHI::EMemoryPropertyFlags memFlags = RHI::EMemoryPropertyBit::DeviceLocal;

#if defined(SAILOR_VULKAN_STORE_VERTICES_INDICES_IN_SSBO)
	auto& ssboAllocator = GetMeshSsboAllocator();

	mesh->m_vertexBuffer = RHI::RHIBufferPtr::Make(flags, memFlags);
	mesh->m_indexBuffer = RHI::RHIBufferPtr::Make(flags, memFlags);

	mesh->m_vertexBuffer->m_vulkan.m_buffer = ssboAllocator->Allocate(bufferSize, mesh->m_vertexDescription->GetVertexStride());
	mesh->m_vertexBuffer->m_vulkan.m_bufferAllocator = ssboAllocator;

	mesh->m_indexBuffer->m_vulkan.m_buffer = ssboAllocator->Allocate(indexBufferSize, device->GetMinSsboOffsetAlignment());
	mesh->m_indexBuffer->m_vulkan.m_bufferAllocator = ssboAllocator;

	UpdateBuffer(cmdList, mesh->m_vertexBuffer, pVertices, bufferSize);
	UpdateBuffer(cmdList, mesh->m_indexBuffer, pIndices, indexBufferSize);
#else
	mesh->m_vertexBuffer = CreateBuffer(cmdList,
		pVertices,
		bufferSize,
		flags,
		memFlags);

	mesh->m_indexBuffer = CreateBuffer(cmdList,
		pIndices,
		indexBufferSize,
		flags,
		memFlags);
#endif

	RHI::Renderer::GetDriverCommands()->EndCommandList(cmdList);

	// Create fences to track the state of mesh creation
	RHI::RHIFencePtr fence = RHI::RHIFencePtr::Make();
	RHI::Renderer::GetDriver()->SetDebugName(fence, "Update mesh");

	TrackDelayedInitialization(mesh.GetRawPtr(), fence);

	// Submit cmd lists
	SAILOR_ENQUEUE_TASK_RENDER_THREAD("Create mesh",
		([this, cmdList, fence]()
			{
				SubmitCommandList(cmdList, fence);
			}));
}

void VulkanGraphicsDriver::Dispatch(RHI::RHICommandListPtr cmd,
	RHI::RHIShaderPtr computeShader,
	uint32_t groupSizeX, uint32_t groupSizeY, uint32_t groupSizeZ,
	const TVector<RHI::RHIShaderBindingSetPtr>& bindings,
	const void* pPushConstantsData,
	uint32_t sizePushConstantsData)
{
	check(computeShader->GetStage() == RHI::EShaderStage::Compute);

	if (computeShader->GetStage() != RHI::EShaderStage::Compute)
	{
		return;
	}

	VulkanComputePipelinePtr computePipeline = GetOrAddComputePipeline(computeShader, sizePushConstantsData);
	const TVector<VulkanDescriptorSetPtr>& sets = GetCompatibleDescriptorSets(computePipeline->m_layout, bindings);

	if (pPushConstantsData && sizePushConstantsData > 0)
	{
		cmd->m_vulkan.m_commandBuffer->PushConstants(computePipeline->m_layout, 0, sizePushConstantsData, pPushConstantsData);
	}

	cmd->m_vulkan.m_commandBuffer->BindDescriptorSet(computePipeline->m_layout, sets, VK_PIPELINE_BIND_POINT_COMPUTE);
	cmd->m_vulkan.m_commandBuffer->BindPipeline(computePipeline);
	cmd->m_vulkan.m_commandBuffer->Dispatch(groupSizeX, groupSizeY, groupSizeZ);
	cmd->m_vulkan.m_commandBuffer->AddDependency(computeShader);
}

void VulkanGraphicsDriver::BindMaterial(RHI::RHICommandListPtr cmd, RHI::RHIMaterialPtr material)
{
	auto pipeline = material->m_vulkan.GetOrAddPipeline(cmd->m_vulkan.m_commandBuffer->GetCurrentColorAttachments(),
		(VkFormat)cmd->m_vulkan.m_commandBuffer->GetCurrentDepthAttachment());

	cmd->m_vulkan.m_commandBuffer->BindPipeline(pipeline);
	cmd->m_vulkan.m_commandBuffer->SetDepthBias(material->GetRenderState().GetDepthBias());
}

void VulkanGraphicsDriver::BindVertexBuffer(RHI::RHICommandListPtr cmd, RHI::RHIBufferPtr vertexBuffer, uint32_t offset)
{
	auto ptr = *vertexBuffer->m_vulkan.m_buffer;
	TVector<VulkanBufferPtr> buffers{ ptr.m_buffer };

	//The offset is handled by RHI::RHIBuffer
	cmd->m_vulkan.m_commandBuffer->BindVertexBuffers(buffers, { offset });
}

void VulkanGraphicsDriver::BindIndexBuffer(RHI::RHICommandListPtr cmd, RHI::RHIBufferPtr indexBuffer, uint32_t offset, bool bUint16InsteadOfUint32)
{
	cmd->m_vulkan.m_commandBuffer->BindIndexBuffer(indexBuffer->m_vulkan.m_buffer.m_ptr.m_buffer, offset, bUint16InsteadOfUint32);
}

void VulkanGraphicsDriver::SetViewport(RHI::RHICommandListPtr cmd, float x, float y, float width, float height, glm::vec2 scissorOffset, glm::vec2  scissorExtent, float minDepth, float maxDepth)
{
	VulkanStateViewportPtr pStateViewport = new VulkanStateViewport(x, y, width, height,
		VkOffset2D((int32_t)scissorOffset.x, (int32_t)scissorOffset.y),
		VkExtent2D((uint32_t)scissorExtent.x, (uint32_t)scissorExtent.y), minDepth, maxDepth);

	cmd->m_vulkan.m_commandBuffer->SetViewport(pStateViewport);
	cmd->m_vulkan.m_commandBuffer->SetScissor(pStateViewport);
}

void VulkanGraphicsDriver::SetDefaultViewport(RHI::RHICommandListPtr cmd)
{
	auto device = m_vkInstance->GetMainDevice();
	auto pStateViewport = device->GetCurrentFrameViewport();

	cmd->m_vulkan.m_commandBuffer->SetViewport(pStateViewport);
	cmd->m_vulkan.m_commandBuffer->SetScissor(pStateViewport);
}

TVector<VulkanDescriptorSetPtr> VulkanGraphicsDriver::GetCompatibleDescriptorSets(VulkanPipelineLayoutPtr layout, const TVector<RHI::RHIShaderBindingSetPtr>& shaderBindings)
{
	SAILOR_PROFILE_FUNCTION();

	TVector<VulkanDescriptorSetPtr> descriptorSets;
	descriptorSets.Reserve(shaderBindings.Num());

	const TVector<bool> bIsCompatible = IsCompatible(layout, shaderBindings);
	auto device = m_vkInstance->GetMainDevice();

	for (uint32_t i = 0; i < shaderBindings.Num(); i++)
	{
		if (i >= layout->m_descriptionSetLayouts.Num())
		{
			break;
		}

		const auto& materialLayout = layout->m_descriptionSetLayouts[i];

		if (bIsCompatible[i])
		{
			descriptorSets.Add(shaderBindings[i]->m_vulkan.m_descriptorSet);
			continue;
		}

		CachedDescriptorSet cache = CachedDescriptorSet(layout, shaderBindings[i]);

		// Flush lifetime
		auto& cachedDS = m_cachedDescriptorSets.At_Lock(cache);
		cachedDS.m_second = 0;
		VulkanDescriptorSetPtr& cachedDescriptorSet = cachedDS.m_first;

		if (cachedDescriptorSet.IsValid())
		{
			descriptorSets.Add(cachedDescriptorSet);
			m_cachedDescriptorSets.Unlock(cache);
			continue;
		}

		TVector<VulkanDescriptorPtr> descriptors;
		TVector<VkDescriptorSetLayoutBinding> descriptionSetLayouts;
		size_t numDescriptors = 0;

		{
			SAILOR_PROFILE_SCOPE("Track changes: remove extra bindings");
			for (const auto& binding : shaderBindings[i]->GetShaderBindings())
			{
				if (binding.m_second->IsBind())
				{
					if (!materialLayout->m_descriptorSetLayoutBindings.ContainsIf(
						[&](const auto& lhs)
						{
							auto& layout = binding.m_second->GetLayout();
							const bool bIsImage = (VkDescriptorType::VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER == lhs.descriptorType) || (VkDescriptorType::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE == lhs.descriptorType);
							return lhs.binding == layout.m_binding && (layout.IsImage() == bIsImage); })
						)
					{
						// We don't add extra bindings 
						continue;
					}

					if (binding.m_second->GetTextureBindings().Num() > 0)
					{
						uint32_t index = 0;
						for (auto& texture : binding.m_second->GetTextureBindings())
						{
							if (binding.m_second->GetLayout().m_type == RHI::EShaderBindingType::CombinedImageSampler)
							{
								auto descr = VulkanDescriptorCombinedImagePtr::Make(binding.m_second->m_vulkan.m_descriptorSetLayout.binding, index,
									device->GetSamplers()->GetSampler(texture->GetFiltration(), texture->GetClamping(), texture->HasMipMaps(), texture->GetSamplerReduction()),
									texture->m_vulkan.m_imageView);
								descriptors.Add(descr);
							}
							else if (binding.m_second->GetLayout().m_type == RHI::EShaderBindingType::StorageImage)
							{
								auto descr = VulkanDescriptorStorageImagePtr::Make(binding.m_second->m_vulkan.m_descriptorSetLayout.binding, index, texture->m_vulkan.m_imageView);
								descriptors.Add(descr);
							}
							index++;
						}
						descriptionSetLayouts.Add(binding.m_second->m_vulkan.m_descriptorSetLayout);

						numDescriptors++;
					}
					else if (binding.m_second->m_vulkan.m_valueBinding)
					{
						const auto type = binding.m_second->GetLayout().m_type;
						const bool bBindWithoutOffset = (type == RHI::EShaderBindingType::StorageBuffer && !binding.m_second->m_vulkan.m_bBindSsboWithOffset);

						auto& valueBinding = *(binding.m_second->m_vulkan.m_valueBinding->Get());
						auto descr = VulkanDescriptorBufferPtr::Make(binding.m_second->m_vulkan.m_descriptorSetLayout.binding,
							0,
							valueBinding.m_buffer,
							bBindWithoutOffset ? 0 : valueBinding.m_offset,
							valueBinding.m_size,
							binding.m_second->GetLayout().m_type);

						descriptors.Add(descr);
						descriptionSetLayouts.Add(binding.m_second->m_vulkan.m_descriptorSetLayout);

						numDescriptors++;
					}
				}
			}
		}

		{
			SAILOR_PROFILE_SCOPE("Track changes: add missing combined sampler and storage images");

			TVector<RHI::RHIShaderBindingPtr> bindings = shaderBindings[i]->GetShaderBindings().GetValues();

			if (materialLayout->m_descriptorSetLayoutBindings.Num() > numDescriptors)
			{
				for (uint32_t j = 0; j < materialLayout->m_descriptorSetLayoutBindings.Num(); j++)
				{
					const auto& descrSetLayoutBinding = materialLayout->m_descriptorSetLayoutBindings[j];

					if (!(descrSetLayoutBinding.descriptorType == (VkDescriptorType)RHI::EShaderBindingType::CombinedImageSampler ||
						descrSetLayoutBinding.descriptorType == (VkDescriptorType)RHI::EShaderBindingType::StorageImage))
					{
						continue;
					}

					if (!bindings.ContainsIf([&](const auto& lhs)
						{
							auto& layout = lhs->GetLayout();
							return descrSetLayoutBinding.binding == layout.m_binding;
						}))
					{
						// Check that bound map is Cubemap
						size_t index = layout->GetShaderLayout().FindIf([&](const auto& lhs) {return i == lhs.m_set && descrSetLayoutBinding.binding == lhs.m_binding; });
						const bool bIsCubemap = index != -1 ? layout->GetShaderLayout()[index].IsCubemap() : false;

						if (descrSetLayoutBinding.descriptorType == (VkDescriptorType)RHI::EShaderBindingType::CombinedImageSampler)
						{
							auto descr = VulkanDescriptorCombinedImagePtr::Make(descrSetLayoutBinding.binding, 0,
								device->GetSamplers()->GetSampler(RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Repeat, false, RHI::ESamplerReductionMode::Average),
								bIsCubemap ? m_vkDefaultCubemap : m_vkDefaultTexture);
							descriptors.Add(descr);
						}
						else if (descrSetLayoutBinding.descriptorType == (VkDescriptorType)RHI::EShaderBindingType::StorageImage)
						{
							auto descr = VulkanDescriptorStorageImagePtr::Make(descrSetLayoutBinding.binding, 0, bIsCubemap ? m_vkDefaultCubemap : m_vkDefaultTexture);
							descriptors.Add(descr);
						}

						descriptionSetLayouts.Add(descrSetLayoutBinding);

						if (materialLayout->m_descriptorSetLayoutBindings.Num() == descriptors.Num())
						{
							break;
						}
					}
				}
			}
		}

		VulkanDescriptorSetPtr descriptorSet;
		{
			SAILOR_PROFILE_SCOPE("Create new descriptor sets");

			descriptorSet = VulkanDescriptorSetPtr::Make(device,
				device->GetCurrentThreadContext().m_descriptorPool,
				VulkanDescriptorSetLayoutPtr::Make(device, descriptionSetLayouts),
				descriptors);
		}
#ifndef _SHIPPING
		if (VkDescriptorSet handleSet = *descriptorSet)
		{
			m_vkInstance->GetMainDevice()->SetDebugName(VkObjectType::VK_OBJECT_TYPE_DESCRIPTOR_SET, (uint64_t)handleSet, "Compatible Cache Descriptor Set");
		}
#endif

		{
			SAILOR_PROFILE_SCOPE("Compile new descriptor sets");
			descriptorSet->Compile();
			descriptorSets.Add(descriptorSet);

			cachedDescriptorSet = descriptorSets[i];
			m_cachedDescriptorSets.Unlock(cache);
		}
	}

	return descriptorSets;
}

bool VulkanGraphicsDriver::CachedDescriptorSet::IsExpired() const
{
	return !m_layout.IsShared() || !m_binding.IsShared() || m_initialCompatibility != m_binding->GetCompatibilityHashCode();
}

VulkanGraphicsDriver::CachedDescriptorSet& VulkanGraphicsDriver::CachedDescriptorSet::operator=(const CachedDescriptorSet& rhs)
{
	m_layout = rhs.m_layout;
	m_binding = rhs.m_binding;

	m_initialCompatibility = m_binding->GetCompatibilityHashCode();

	return *this;
}

bool VulkanGraphicsDriver::CachedDescriptorSet::operator==(const CachedDescriptorSet& rhs) const
{
	return m_layout == rhs.m_layout && m_binding == rhs.m_binding && m_initialCompatibility == m_binding->GetCompatibilityHashCode();
}

size_t VulkanGraphicsDriver::CachedDescriptorSet::GetHash() const
{
	return m_initialCompatibility;
}

VulkanGraphicsDriver::CachedDescriptorSet::CachedDescriptorSet(const VulkanPipelineLayoutPtr& material, const RHI::RHIShaderBindingSetPtr& binding) noexcept :
	m_layout(material),
	m_binding(binding)
{
	m_initialCompatibility = m_binding->GetCompatibilityHashCode();
}

void VulkanGraphicsDriver::BindShaderBindings(RHI::RHICommandListPtr cmd, RHI::RHIMaterialPtr material, const TVector<RHI::RHIShaderBindingSetPtr>& bindings)
{
	SAILOR_PROFILE_FUNCTION();

	const TVector<VulkanDescriptorSetPtr>& sets = GetCompatibleDescriptorSets(material->m_vulkan.m_pipelines[0]->m_layout, bindings);
	cmd->m_vulkan.m_commandBuffer->BindDescriptorSet(material->m_vulkan.m_pipelines[0]->m_layout, sets, VK_PIPELINE_BIND_POINT_GRAPHICS);

	// Need to handle ShaderBindingSet, since it auto destructs all bindings and buffers
	for (const auto& dep : bindings)
	{
		cmd->m_vulkan.m_commandBuffer->AddDependency(dep);
	}
}

void VulkanGraphicsDriver::DrawIndexedIndirect(RHI::RHICommandListPtr cmd, RHI::RHIBufferPtr buffer, size_t offset, uint32_t drawCount, uint32_t stride)
{
	auto mainDevice = m_vkInstance->GetMainDevice();
	if (mainDevice->IsMultiDrawIndirectSupported())
	{
		cmd->m_vulkan.m_commandBuffer->DrawIndexedIndirect(*buffer->m_vulkan.m_buffer, offset, drawCount, stride);
	}
	else
	{
		for (size_t j = 0; j < drawCount; j++)
		{
			cmd->m_vulkan.m_commandBuffer->DrawIndexedIndirect(*buffer->m_vulkan.m_buffer, offset + j * stride, 1, stride);
		}
	}
}

void VulkanGraphicsDriver::DrawIndexed(RHI::RHICommandListPtr cmd, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, uint32_t vertexOffset, uint32_t firstInstance)
{
	cmd->m_vulkan.m_commandBuffer->DrawIndexed(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

bool VulkanGraphicsDriver::FitsViewport(RHI::RHICommandListPtr cmd, float x, float y, float width, float height, glm::vec2 scissorOffset, glm::vec2 scissorExtent, float minDepth, float maxDepth)
{
	VulkanStateViewport viewport(x, y, width, height,
		VkOffset2D((int32_t)scissorOffset.x, (int32_t)scissorOffset.y),
		VkExtent2D((uint32_t)scissorExtent.x, (uint32_t)scissorExtent.y),
		minDepth, maxDepth);

	return cmd->m_vulkan.m_commandBuffer->FitsViewport(viewport.GetViewport());
}

bool VulkanGraphicsDriver::FitsDefaultViewport(RHI::RHICommandListPtr cmd)
{
	auto device = m_vkInstance->GetMainDevice();
	auto pStateViewport = device->GetCurrentFrameViewport();

	return cmd->m_vulkan.m_commandBuffer->FitsViewport(pStateViewport->GetViewport());
}

void VulkanGraphicsDriver::CollectGarbage_RenderThread()
{
	SAILOR_PROFILE_FUNCTION();

	m_cachedDescriptorSets.LockAll();
	TSet<CachedDescriptorSet> toRemove;

	for (auto& batch : m_cachedDescriptorSets)
	{
		if (!batch.m_second.m_first || batch.m_first.IsExpired() || ++batch.m_second.m_second > CachedDescriptorSetLifeTimeInFrames)
		{
			toRemove.Insert(batch.m_first);
		}
	}

	for (const auto& remove : toRemove)
	{
		m_cachedDescriptorSets.ForcelyRemove(remove);
	}
	m_cachedDescriptorSets.UnlockAll();
}

void VulkanGraphicsDriver::StartGpuTracking()
{
	m_lastFrameGpuStats.m_barriers.Clear();
	m_bIsTrackingGpu = true;
}

RHI::GpuStats VulkanGraphicsDriver::FinishGpuTracking()
{
	m_bIsTrackingGpu = false;
	return std::move(m_lastFrameGpuStats);
}