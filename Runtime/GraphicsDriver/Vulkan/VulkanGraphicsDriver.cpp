#include "VulkanGraphicsDriver.h"
#include "RHI/Texture.h"
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
#include "AssetRegistry/Shader/ShaderCompiler.h"

using namespace Sailor;
using namespace Sailor::GraphicsDriver::Vulkan;

void VulkanGraphicsDriver::Initialize(const Win32::Window* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug)
{
	GraphicsDriver::Vulkan::VulkanApi::Initialize(pViewport, msaaSamples, bIsDebug);
	m_vkInstance = GraphicsDriver::Vulkan::VulkanApi::GetInstance();

	m_backBuffer = RHI::RHITexturePtr::Make(RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Repeat, false, RHI::EImageLayout::PresentSrc);
	m_depthStencilBuffer = RHI::RHITexturePtr::Make(RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Repeat, false, RHI::EImageLayout::DepthStencilAttachmentOptimal);
}

VulkanGraphicsDriver::~VulkanGraphicsDriver()
{
	m_cachedMsaaRenderTargets.Clear();
	m_cachedComputePipelines.Clear();
	m_cachedDescriptorSets.Clear();
	m_backBuffer.Clear();
	m_depthStencilBuffer.Clear();

	TrackResources_ThreadSafe();

	m_trackedFences.Clear();
	m_uniformBuffers.Clear();
	m_materialSsboAllocator.Clear();
	m_meshSsboAllocator.Clear();

	// Waiting finishing releasing of rendering resources
	App::GetSubmodule<Tasks::Scheduler>()->WaitIdle(Tasks::EThreadType::Render);
	App::GetSubmodule<Tasks::Scheduler>()->WaitIdle(Tasks::EThreadType::RHI);

	GraphicsDriver::Vulkan::VulkanApi::Shutdown();
}

uint32_t VulkanGraphicsDriver::GetNumSubmittedCommandBuffers() const
{
	return m_vkInstance->GetMainDevice()->GetNumSubmittedCommandBufers();
}

bool VulkanGraphicsDriver::ShouldFixLostDevice(const Win32::Window* pViewport)
{
	return m_vkInstance->GetMainDevice()->ShouldFixLostDevice(pViewport);
}

bool VulkanGraphicsDriver::FixLostDevice(const Win32::Window* pViewport)
{
	SAILOR_PROFILE_FUNCTION();

	if (m_vkInstance->GetMainDevice()->ShouldFixLostDevice(pViewport))
	{
		SAILOR_PROFILE_BLOCK("Fix lost device");

		m_vkInstance->WaitIdle();
		m_vkInstance->GetMainDevice()->FixLostDevice(pViewport);

		SAILOR_PROFILE_END_BLOCK();

		if (App::GetViewportWindow()->IsIconic())
		{
			m_cachedMsaaRenderTargets.Clear();
		}
		else
		{
			SAILOR_ENQUEUE_TASK_RENDER_THREAD("Clear MSAA cache",
				([this]()
			{
				m_cachedMsaaRenderTargets.Clear();
			}));
		}

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

RHI::RHITexturePtr VulkanGraphicsDriver::GetBackBuffer() const
{
	return m_backBuffer;
}

RHI::RHITexturePtr VulkanGraphicsDriver::GetDepthBuffer() const
{
	return m_depthStencilBuffer;
}

bool VulkanGraphicsDriver::AcquireNextImage()
{
	const bool bRes = m_vkInstance->GetMainDevice()->AcquireNextImage();

	m_backBuffer->m_vulkan.m_imageView = m_vkInstance->GetMainDevice()->GetBackBuffer();
	m_backBuffer->m_vulkan.m_image = m_backBuffer->m_vulkan.m_imageView->m_image;

	m_depthStencilBuffer->m_vulkan.m_imageView = m_vkInstance->GetMainDevice()->GetDepthBuffer();
	m_depthStencilBuffer->m_vulkan.m_image = m_depthStencilBuffer->m_vulkan.m_imageView->m_image;

	return bRes;
}

bool VulkanGraphicsDriver::PresentFrame(const class FrameState& state,
	const TVector<RHI::RHICommandListPtr>& primaryCommandBuffers,
	const TVector<RHI::RHISemaphorePtr>& waitSemaphores) const
{
	TVector<VulkanCommandBufferPtr> primaryBuffers;
	TVector<VulkanSemaphorePtr> vkWaitSemaphores;

	primaryBuffers.Reserve(primaryCommandBuffers.Num());
	for (auto& buffer : primaryCommandBuffers)
	{
		primaryBuffers.Add(buffer->m_vulkan.m_commandBuffer);
	}

	vkWaitSemaphores.Reserve(waitSemaphores.Num());
	for (auto& buffer : waitSemaphores)
	{
		vkWaitSemaphores.Add(buffer->m_vulkan.m_semaphore);
	}

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

	SAILOR_PROFILE_BLOCK("Check fence");

	//if we have fence and that is null we should create device resource
	if (fence && !fence->m_vulkan.m_fence)
	{
		fence->m_vulkan.m_fence = VulkanFencePtr::Make(m_vkInstance->GetMainDevice());
	}

	SAILOR_PROFILE_END_BLOCK();

	TVector<VulkanSemaphorePtr> signal;
	TVector<VulkanSemaphorePtr> wait;

	SAILOR_PROFILE_BLOCK("Add semaphore dependencies");

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

	SAILOR_PROFILE_END_BLOCK();

	m_vkInstance->GetMainDevice()->SubmitCommandBuffer(commandList->m_vulkan.m_commandBuffer, fence ? fence->m_vulkan.m_fence : nullptr, signal, wait);

	if (fence)
	{
		SAILOR_PROFILE_BLOCK("Add fence dependencies");

		// Fence should hold command list during execution
		fence->AddDependency(commandList);

		// We should remove fence after execution
		TrackPendingCommandList_ThreadSafe(fence);

		SAILOR_PROFILE_END_BLOCK();
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

RHI::RHICommandListPtr VulkanGraphicsDriver::CreateCommandList(bool bIsSecondary, bool bOnlyTransferQueue)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	RHI::RHICommandListPtr cmdList = RHI::RHICommandListPtr::Make();

	cmdList->m_vulkan.m_commandBuffer = VulkanCommandBufferPtr::Make(device,
		bOnlyTransferQueue ? device->GetCurrentThreadContext().m_transferCommandPool : device->GetCurrentThreadContext().m_commandPool,
		bIsSecondary ? VK_COMMAND_BUFFER_LEVEL_SECONDARY : VK_COMMAND_BUFFER_LEVEL_PRIMARY);

	return cmdList;
}

RHI::RHIBufferPtr VulkanGraphicsDriver::CreateIndirectBuffer(size_t size)
{
	SAILOR_PROFILE_FUNCTION();

	const uint32_t usage = RHI::EBufferUsageBit::IndirectBuffer_Bit | RHI::EBufferUsageBit::BufferTransferDst_Bit | RHI::EBufferUsageBit::BufferTransferSrc_Bit;

	RHI::RHIBufferPtr res = RHI::RHIBufferPtr::Make(usage);
	auto buffer = m_vkInstance->CreateBuffer(m_vkInstance->GetMainDevice(), size, (uint16_t)usage, VkMemoryPropertyFlagBits::VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

	// Hack to store ordinary buffer in TMemoryPtr
	res->m_vulkan.m_buffer = TMemoryPtr<VulkanBufferMemoryPtr>(0, 0, buffer->m_size, VulkanBufferMemoryPtr(buffer, 0, buffer->m_size), -1);

	return res;
}

RHI::RHIBufferPtr VulkanGraphicsDriver::CreateBuffer(size_t size, RHI::EBufferUsageFlags usage)
{
	SAILOR_PROFILE_FUNCTION();

	RHI::RHIBufferPtr res = RHI::RHIBufferPtr::Make(usage);
	auto buffer = m_vkInstance->CreateBuffer(m_vkInstance->GetMainDevice(), size, (uint16_t)usage, VkMemoryPropertyFlagBits::VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	// Hack to store ordinary buffer in TMemoryPtr
	res->m_vulkan.m_buffer = TMemoryPtr<VulkanBufferMemoryPtr>(0, 0, buffer->m_size, VulkanBufferMemoryPtr(buffer, 0, buffer->m_size), -1);

	return res;
}

RHI::RHIBufferPtr VulkanGraphicsDriver::CreateBuffer(RHI::RHICommandListPtr& cmdList, const void* pData, size_t size, RHI::EBufferUsageFlags usage)
{
	SAILOR_PROFILE_FUNCTION();

	RHI::RHIBufferPtr outBuffer = CreateBuffer(size, usage);

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

	RHI::RHIBufferPtr res = RHI::RHIBufferPtr::Make(usage);
	auto buffer = m_vkInstance->CreateBuffer_Immediate(m_vkInstance->GetMainDevice(), pData, size, (uint32_t)usage);

	// Hack to store ordinary buffer in TMemoryPtr
	res->m_vulkan.m_buffer = TMemoryPtr<VulkanBufferMemoryPtr>(0, 0, buffer->m_size, VulkanBufferMemoryPtr(buffer, 0, buffer->m_size), -1);
	return res;
}

void VulkanGraphicsDriver::CopyBuffer_Immediate(RHI::RHIBufferPtr src, RHI::RHIBufferPtr dst, size_t size)
{
	m_vkInstance->CopyBuffer_Immediate(m_vkInstance->GetMainDevice(), *src->m_vulkan.m_buffer, *dst->m_vulkan.m_buffer, size);
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
		(VkImageLayout)RHI::EImageLayout::ShaderReadOnlyOptimal);

	res->m_vulkan.m_imageView = VulkanImageViewPtr::Make(device, res->m_vulkan.m_image);
	res->m_vulkan.m_imageView->Compile();

	return res;
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
	RHI::ETextureUsageFlags usage)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();
	RHI::RHITexturePtr outTexture = RHI::RHITexturePtr::Make(filtration, clamping, mipLevels > 1, RHI::EImageLayout::ShaderReadOnlyOptimal);

	VkExtent3D vkExtent;
	vkExtent.width = extent.x;
	vkExtent.height = extent.y;
	vkExtent.depth = extent.z;

	RHI::RHICommandListPtr cmdList = RHI::Renderer::GetDriver()->CreateCommandList(false, false);
	RHI::Renderer::GetDriverCommands()->BeginCommandList(cmdList, true);

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
		(VkImageLayout)RHI::EImageLayout::ShaderReadOnlyOptimal);

	RHI::Renderer::GetDriverCommands()->EndCommandList(cmdList);

	outTexture->m_vulkan.m_imageView = VulkanImageViewPtr::Make(device, outTexture->m_vulkan.m_image);
	outTexture->m_vulkan.m_imageView->Compile();
	outTexture->m_vulkan.m_image->m_defaultLayout = (VkImageLayout)(outTexture->GetDefaultLayout());

	RHI::RHIFencePtr fenceUpdateRes = RHI::RHIFencePtr::Make();
	TrackDelayedInitialization(outTexture.GetRawPtr(), fenceUpdateRes);
	SubmitCommandList(cmdList, fenceUpdateRes);

	return outTexture;
}

RHI::RHITexturePtr VulkanGraphicsDriver::CreateRenderTarget(
	glm::ivec3 extent,
	uint32_t mipLevels,
	RHI::ETextureType type,
	RHI::ETextureFormat format,
	RHI::ETextureFiltration filtration,
	RHI::ETextureClamping clamping,
	RHI::ETextureUsageFlags usage)
{
	SAILOR_PROFILE_FUNCTION();

	VkImageLayout layout = (VkImageLayout)RHI::EImageLayout::General;

	if (const bool bIsUsedAsStorage = usage & VK_IMAGE_USAGE_STORAGE_BIT)
	{
		layout = (VkImageLayout)RHI::EImageLayout::General;
	}
	else if (const bool bIsUsedAsColorAttachment = usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
	{
		layout = (VkImageLayout)RHI::EImageLayout::ColorAttachmentOptimal;
	}

	// Not optimal code
	//assert(layout != (VkImageLayout)RHI::EImageLayout::General);

	auto device = m_vkInstance->GetMainDevice();
	RHI::RHITexturePtr outTexture = RHI::RHITexturePtr::Make(filtration, clamping, false, (RHI::EImageLayout)layout);

	VkExtent3D vkExtent;
	vkExtent.width = extent.x;
	vkExtent.height = extent.y;
	vkExtent.depth = extent.z;

	outTexture->m_vulkan.m_image = m_vkInstance->CreateImage(m_vkInstance->GetMainDevice(),
		vkExtent,
		mipLevels,
		(VkImageType)type,
		(VkFormat)format,
		VK_IMAGE_TILING_OPTIMAL,
		(uint32_t)usage,
		VkSharingMode::VK_SHARING_MODE_EXCLUSIVE,
		VkSampleCountFlagBits::VK_SAMPLE_COUNT_1_BIT,
		layout);

	outTexture->m_vulkan.m_image->m_defaultLayout = (VkImageLayout)(outTexture->GetDefaultLayout());
	outTexture->m_vulkan.m_imageView = VulkanImageViewPtr::Make(device, outTexture->m_vulkan.m_image);
	outTexture->m_vulkan.m_imageView->Compile();

	// Update layout
	RHI::RHICommandListPtr cmdList = RHI::Renderer::GetDriver()->CreateCommandList(false, false);
	RHI::Renderer::GetDriverCommands()->BeginCommandList(cmdList, true);
	RHI::Renderer::GetDriverCommands()->ImageMemoryBarrier(cmdList,
		outTexture,
		outTexture->GetFormat(),
		RHI::EImageLayout::Undefined,
		(RHI::EImageLayout)layout);
	RHI::Renderer::GetDriverCommands()->EndCommandList(cmdList);

	RHI::RHIFencePtr fenceUpdateRes = RHI::RHIFencePtr::Make();
	TrackDelayedInitialization(outTexture.GetRawPtr(), fenceUpdateRes);
	SubmitCommandList(cmdList, fenceUpdateRes);

	return outTexture;
}

RHI::RHISurfacePtr VulkanGraphicsDriver::CreateSurface(
	glm::ivec3 extent,
	uint32_t mipLevels,
	RHI::ETextureType type,
	RHI::ETextureFormat format,
	RHI::ETextureFiltration filtration,
	RHI::ETextureClamping clamping,
	RHI::ETextureUsageFlags usage)
{
	SAILOR_PROFILE_FUNCTION();

	RHI::RHISurfacePtr res;

	auto device = m_vkInstance->GetMainDevice();

	const RHI::RHITexturePtr resolved = CreateRenderTarget(extent, mipLevels, type, format, filtration, clamping, usage);
	RHI::RHITexturePtr target = resolved;

	bool bNeedsResolved = m_vkInstance->GetMainDevice()->GetCurrentMsaaSamples() != VK_SAMPLE_COUNT_1_BIT;
	if (bNeedsResolved)
	{
		target = RHI::RHITexturePtr::Make(filtration, clamping, false, RHI::EImageLayout::ColorAttachmentOptimal);

		VkExtent3D vkExtent;
		vkExtent.width = extent.x;
		vkExtent.height = extent.y;
		vkExtent.depth = extent.z;

		// Disable storage for MSAA target
		usage &= ~(1 << RHI::ETextureUsageBit::Storage_Bit);

		target->m_vulkan.m_image = m_vkInstance->CreateImage(m_vkInstance->GetMainDevice(),
			vkExtent,
			mipLevels,
			(VkImageType)type,
			(VkFormat)format,
			VK_IMAGE_TILING_OPTIMAL,
			(uint32_t)usage,
			(VkSharingMode)VkSharingMode::VK_SHARING_MODE_EXCLUSIVE,
			device->GetCurrentMsaaSamples(),
			(VkImageLayout)RHI::EImageLayout::ColorAttachmentOptimal);

		target->m_vulkan.m_image->m_defaultLayout = (VkImageLayout)(target->GetDefaultLayout());

		target->m_vulkan.m_imageView = VulkanImageViewPtr::Make(device, target->m_vulkan.m_image);
		target->m_vulkan.m_imageView->Compile();


		RHI::RHICommandListPtr cmdList = RHI::Renderer::GetDriver()->CreateCommandList(false, false);
		RHI::Renderer::GetDriverCommands()->BeginCommandList(cmdList, true);
		RHI::Renderer::GetDriverCommands()->ImageMemoryBarrier(cmdList,
			target,
			target->GetFormat(),
			RHI::EImageLayout::Undefined,
			RHI::EImageLayout::ColorAttachmentOptimal);
		RHI::Renderer::GetDriverCommands()->EndCommandList(cmdList);

		RHI::RHIFencePtr fenceUpdateRes = RHI::RHIFencePtr::Make();
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
			if (binding.m_second->GetTextureBinding())
			{
				auto& texture = binding.m_second->GetTextureBinding();

				if (binding.m_second->GetLayout().m_type == RHI::EShaderBindingType::CombinedImageSampler)
				{
					auto descr = VulkanDescriptorCombinedImagePtr::Make(binding.m_second->m_vulkan.m_descriptorSetLayout.binding, 0,
						device->GetSamplers()->GetSampler(texture->GetFiltration(), texture->GetClamping(), texture->ShouldGenerateMips()),
						texture->m_vulkan.m_imageView);
					descriptors.Add(descr);
				}
				else if (binding.m_second->GetLayout().m_type == RHI::EShaderBindingType::StorageImage)
				{
					auto descr = VulkanDescriptorStorageImagePtr::Make(binding.m_second->m_vulkan.m_descriptorSetLayout.binding, 0, texture->m_vulkan.m_imageView);
					descriptors.Add(descr);
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
	bindings->m_vulkan.m_descriptorSet = VulkanDescriptorSetPtr::Make(device,
		device->GetCurrentThreadContext().m_descriptorPool,
		VulkanDescriptorSetLayoutPtr::Make(device, descriptionSetLayouts),
		descriptors);

	bindings->m_vulkan.m_descriptorSet->Compile();
}

RHI::RHIMaterialPtr VulkanGraphicsDriver::CreateMaterial(const RHI::RHIVertexDescriptionPtr& vertexDescription, RHI::EPrimitiveTopology topology, const RHI::RenderState& renderState, const Sailor::ShaderSetPtr& shader)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	TVector<VulkanDescriptorSetLayoutPtr> descriptorSetLayouts;
	TVector<RHI::ShaderLayoutBinding> bindings;

	// We need debug shaders to get full names from reflection
	VulkanApi::CreateDescriptorSetLayouts(device, { shader->GetDebugVertexShaderRHI()->m_vulkan.m_shader, shader->GetDebugFragmentShaderRHI()->m_vulkan.m_shader },
		descriptorSetLayouts, bindings);

	SAILOR_PROFILE_BLOCK("Create and update shader bindings");
	// TODO: move initialization to external code
	auto shaderBindings = CreateShaderBindings();
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
			binding->m_vulkan.m_valueBinding = TManagedMemoryPtr<VulkanBufferMemoryPtr, VulkanBufferAllocator>::Make(storageAllocator->Allocate(layoutBinding.m_size, layoutBinding.m_size), storageAllocator);
			binding->m_vulkan.m_descriptorSetLayout = vkLayoutBinding;

			assert(((*(binding->m_vulkan.m_valueBinding->Get())).m_offset % layoutBinding.m_size) == 0);
			uint32_t instanceIndex = (uint32_t)((*(binding->m_vulkan.m_valueBinding->Get())).m_offset / layoutBinding.m_size);

			binding->m_vulkan.m_storageInstanceIndex = instanceIndex;
			binding->SetLayout(layoutBinding);
		}
	}
	SAILOR_PROFILE_END_BLOCK();

	shaderBindings->SetLayoutShaderBindings(bindings);
	UpdateDescriptorSet(shaderBindings);

	return CreateMaterial(vertexDescription, topology, renderState, shader, shaderBindings);
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

	auto vertex = shader->GetVertexShaderRHI();
	auto fragment = shader->GetFragmentShaderRHI();

	RHI::RHIMaterialPtr res = RHI::RHIMaterialPtr::Make(renderState, vertex, fragment);

	TVector<VkPushConstantRange> pushConstants;

	for (const auto& pushConstant : shader->GetDebugVertexShaderRHI()->m_vulkan.m_shader->GetPushConstants())
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
		pushConstants,
		0);

	auto colorAttachments = shader->GetColorAttachments().ToVector<VkFormat>();
	if (colorAttachments.Num() == 0)
	{
		colorAttachments.Add(device->GetColorFormat());
	}
	else if (colorAttachments.Num() == 1 && colorAttachments[0] == VkFormat::VK_FORMAT_UNDEFINED)
	{
		colorAttachments.Clear();
	}

	auto depthStencilFormat = (VkFormat)shader->GetDepthStencilAttachment();
	if (depthStencilFormat == VkFormat::VK_FORMAT_UNDEFINED)
	{
		depthStencilFormat = device->GetDepthFormat();
	}

	res->m_vulkan.m_pipeline = VulkanGraphicsPipelinePtr::Make(device,
		pipelineLayout,
		TVector{ vertex->m_vulkan.m_shader, fragment->m_vulkan.m_shader },
		device->GetPipelineBuilder()->BuildPipeline(vertexDescription, topology, renderState, colorAttachments, depthStencilFormat),
		0);

	res->m_vulkan.m_pipeline->m_renderPass = device->GetRenderPass();
	res->m_vulkan.m_pipeline->Compile();
	res->SetBindings(shaderBindigs);

	return res;
}

TSharedPtr<VulkanBufferAllocator>& VulkanGraphicsDriver::GetMeshSsboAllocator()
{
	if (!m_meshSsboAllocator)
	{
		// 128 mb is a good point to start
		const size_t StorageBufferBlockSize = 128 * 1024 * 1024u;

		m_meshSsboAllocator = TSharedPtr<VulkanBufferAllocator>::Make(StorageBufferBlockSize, 1024 * 1024u, StorageBufferBlockSize);
		m_meshSsboAllocator->GetGlobalAllocator().SetUsage(RHI::EBufferUsageBit::StorageBuffer_Bit |
			RHI::EBufferUsageBit::VertexBuffer_Bit |
			RHI::EBufferUsageBit::IndexBuffer_Bit |
			VK_BUFFER_USAGE_TRANSFER_DST_BIT);

		m_meshSsboAllocator->GetGlobalAllocator().SetMemoryProperties(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	}
	return m_meshSsboAllocator;
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

	auto& uniformAllocator = m_uniformBuffers[uniformTypeId] = TSharedPtr<VulkanBufferAllocator>::Make();
	uniformAllocator->GetGlobalAllocator().SetUsage(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	uniformAllocator->GetGlobalAllocator().SetMemoryProperties(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	return uniformAllocator;
}

void VulkanGraphicsDriver::UpdateShaderBinding_Immediate(RHI::RHIShaderBindingSetPtr bindings, const std::string& parameter, const void* value, size_t size)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	RHI::RHICommandListPtr commandList = RHI::RHICommandListPtr::Make();
	commandList->m_vulkan.m_commandBuffer = Vulkan::VulkanCommandBufferPtr::Make(device, device->GetCurrentThreadContext().m_commandPool, VkCommandBufferLevel::VK_COMMAND_BUFFER_LEVEL_SECONDARY);

	auto& shaderBinding = bindings->GetOrAddShaderBinding(parameter);
	bool bShouldUpdateDescriptorSet = false;

	// All uniform buffers should be bound
	assert(shaderBinding->IsBind());

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

	pShaderBindings->AddLayoutShaderBinding(layout);
	UpdateDescriptorSet(pShaderBindings);

	return pBinding;
}

RHI::RHIShaderBindingPtr VulkanGraphicsDriver::AddSsboToShaderBindings(RHI::RHIShaderBindingSetPtr& pShaderBindings, const std::string& name, size_t elementSize, size_t numElements, uint32_t shaderBinding, bool bBindSsboWithOffset)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	RHI::RHIShaderBindingPtr binding = pShaderBindings->GetOrAddShaderBinding(name);

	TSharedPtr<VulkanBufferAllocator> allocator = GetMaterialSsboAllocator();

	size_t alignment = elementSize;

	if (bBindSsboWithOffset)
	{
		// We should use the correct alignment
		const size_t reqAlignment = device->GetMinSsboOffsetAlignment();
		alignment = reqAlignment > elementSize ? (reqAlignment % elementSize == 0 ? reqAlignment : elementSize * reqAlignment) : elementSize;
	}

	binding->m_vulkan.m_valueBinding = TManagedMemoryPtr<VulkanBufferMemoryPtr, VulkanBufferAllocator>::Make(allocator->Allocate(elementSize * numElements, alignment), allocator);

	assert((*(binding->m_vulkan.m_valueBinding->Get())).m_offset % elementSize == 0);

	binding->m_vulkan.m_storageInstanceIndex = bBindSsboWithOffset ? 0 : (uint32_t)((*(binding->m_vulkan.m_valueBinding->Get())).m_offset / elementSize);
	binding->m_vulkan.m_bBindSsboWithOffset = bBindSsboWithOffset;

	RHI::ShaderLayoutBinding layout;
	layout.m_binding = shaderBinding;
	layout.m_name = name;
	layout.m_size = (uint32_t)(numElements * elementSize);
	layout.m_type = RHI::EShaderBindingType::StorageBuffer;

	binding->SetLayout(layout);
	binding->m_vulkan.m_descriptorSetLayout = VulkanApi::CreateDescriptorSetLayoutBinding(layout.m_binding, (VkDescriptorType)layout.m_type);

	pShaderBindings->AddLayoutShaderBinding(layout);

	UpdateDescriptorSet(pShaderBindings);

	return binding;
}

RHI::RHIShaderBindingPtr VulkanGraphicsDriver::AddBufferToShaderBindings(RHI::RHIShaderBindingSetPtr& pShaderBindings, const std::string& name, size_t size, uint32_t shaderBinding, RHI::EShaderBindingType bufferType)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	RHI::RHIShaderBindingPtr binding = pShaderBindings->GetOrAddShaderBinding(name);
	TSharedPtr<VulkanBufferAllocator> allocator;

	if (const bool bIsStorage = bufferType == RHI::EShaderBindingType::StorageBuffer)
	{
		allocator = GetMaterialSsboAllocator();
		binding->m_vulkan.m_valueBinding = TManagedMemoryPtr<VulkanBufferMemoryPtr, VulkanBufferAllocator>::Make(allocator->Allocate(size, size), allocator);
		binding->m_vulkan.m_storageInstanceIndex = (uint32_t)((**binding->m_vulkan.m_valueBinding->Get()).m_offset / size);
	}
	else
	{
		allocator = GetUniformBufferAllocator(name);
		binding->m_vulkan.m_valueBinding = TManagedMemoryPtr<VulkanBufferMemoryPtr, VulkanBufferAllocator>::Make(allocator->Allocate(size, device->GetMinUboOffsetAlignment()), allocator);
	}

	RHI::ShaderLayoutBinding layout;
	layout.m_binding = shaderBinding;
	layout.m_name = name;
	layout.m_size = (uint32_t)size;
	layout.m_type = bufferType;

	binding->SetLayout(layout);
	binding->m_vulkan.m_descriptorSetLayout = VulkanApi::CreateDescriptorSetLayoutBinding(layout.m_binding, (VkDescriptorType)layout.m_type);

	pShaderBindings->AddLayoutShaderBinding(layout);

	UpdateDescriptorSet(pShaderBindings);

	return binding;
}

RHI::RHIShaderBindingPtr VulkanGraphicsDriver::AddSamplerToShaderBindings(RHI::RHIShaderBindingSetPtr& pShaderBindings, const std::string& name, RHI::RHITexturePtr texture, uint32_t shaderBinding)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();
	RHI::RHIShaderBindingPtr binding = pShaderBindings->GetOrAddShaderBinding(name);

	RHI::ShaderLayoutBinding layout;
	layout.m_binding = shaderBinding;
	layout.m_name = name;
	layout.m_type = RHI::EShaderBindingType::CombinedImageSampler;

	binding->SetLayout(layout);
	binding->m_vulkan.m_descriptorSetLayout = VulkanApi::CreateDescriptorSetLayoutBinding(layout.m_binding, (VkDescriptorType)layout.m_type);
	binding->SetTextureBinding(texture);

	pShaderBindings->AddLayoutShaderBinding(layout);
	UpdateDescriptorSet(pShaderBindings);

	return binding;
}

RHI::RHIShaderBindingPtr VulkanGraphicsDriver::AddStorageImageToShaderBindings(RHI::RHIShaderBindingSetPtr& pShaderBindings, const std::string& name, RHI::RHITexturePtr texture, uint32_t shaderBinding)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();
	RHI::RHIShaderBindingPtr binding = pShaderBindings->GetOrAddShaderBinding(name);

	RHI::ShaderLayoutBinding layout;
	layout.m_binding = shaderBinding;
	layout.m_name = name;
	layout.m_type = RHI::EShaderBindingType::StorageImage;

	binding->SetLayout(layout);
	binding->m_vulkan.m_descriptorSetLayout = VulkanApi::CreateDescriptorSetLayoutBinding(layout.m_binding, (VkDescriptorType)layout.m_type);
	binding->SetTextureBinding(texture);

	pShaderBindings->AddLayoutShaderBinding(layout);
	UpdateDescriptorSet(pShaderBindings);

	return binding;
}

void VulkanGraphicsDriver::UpdateShaderBinding(RHI::RHIShaderBindingSetPtr bindings, const std::string& parameter, RHI::RHITexturePtr value)
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
		auto& descriptors = bindings->m_vulkan.m_descriptorSet->m_descriptors;
		auto descrIt = std::find_if(descriptors.begin(), descriptors.end(), [=](const VulkanDescriptorPtr& descriptor)
		{
			return descriptor->GetBinding() == layoutBindings[index].m_binding;
		});

		if (descrIt != descriptors.end())
		{
			// Should we fully recreate descriptorSet to avoid race condition?
			//UpdateDescriptorSet(material);

			auto descriptor = (*descrIt).DynamicCast<VulkanDescriptorCombinedImage>();
			descriptor->SetImageView(value->m_vulkan.m_imageView);
			bindings->m_vulkan.m_descriptorSet->Compile();

			return;
		}
		else
		{
			// Add new texture binding
			auto textureBinding = bindings->GetOrAddShaderBinding(parameter);
			textureBinding->SetTextureBinding(value);
			textureBinding->m_vulkan.m_descriptorSetLayout = VulkanApi::CreateDescriptorSetLayoutBinding(layoutBindings[index].m_binding, (VkDescriptorType)layoutBindings[index].m_type);
			UpdateDescriptorSet(bindings);

			return;
		}
	}

	SAILOR_LOG("Trying to update not bind uniform sampler");
}

VulkanComputePipelinePtr VulkanGraphicsDriver::GetOrAddComputePipeline(RHI::RHIShaderPtr computeShader)
{
	auto& computePipeline = m_cachedComputePipelines.At_Lock(computeShader);

	if (!computePipeline)
	{
		auto device = m_vkInstance->GetMainDevice();

		TVector<VulkanDescriptorSetLayoutPtr> descriptorSetLayouts;
		TVector<RHI::ShaderLayoutBinding> bindings;

		// We need debug shaders to get full names from reflection
		VulkanApi::CreateDescriptorSetLayouts(device, { computeShader->m_vulkan.m_shader }, descriptorSetLayouts, bindings);

		TVector<VkPushConstantRange> pushConstants;

		for (const auto& pushConstant : computeShader->m_vulkan.m_shader->GetPushConstants())
		{
			VkPushConstantRange vkPushConstant;
			vkPushConstant.offset = 0;
			vkPushConstant.size = 256;
			vkPushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;

			pushConstants.Emplace(vkPushConstant);
		}

		auto pipelineLayout = VulkanPipelineLayoutPtr::Make(device, descriptorSetLayouts, pushConstants, 0);
		computePipeline = VulkanComputePipelinePtr::Make(device, pipelineLayout, computeShader->m_vulkan.m_shader);
		computePipeline->Compile();
	}

	m_cachedComputePipelines.Unlock(computeShader);

	return computePipeline;
}

RHI::RHITexturePtr VulkanGraphicsDriver::GetOrAddMsaaRenderTarget(RHI::EFormat textureFormat, glm::ivec2 extent)
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
	const auto usage = (uint32_t)(VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT |
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

	return m_cachedMsaaRenderTargets[hash] = target;
}

// IGraphicsDriverCommands
void VulkanGraphicsDriver::PushConstants(RHI::RHICommandListPtr cmd, RHI::RHIMaterialPtr material, size_t size, const void* ptr)
{
	cmd->m_vulkan.m_commandBuffer->PushConstants(material->m_vulkan.m_pipeline->m_layout, 0, size, ptr);
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
	barrier.srcAccessMask = 0; // TODO
	barrier.dstAccessMask = 0; // TODO

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

void VulkanGraphicsDriver::ImageMemoryBarrier(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr image, RHI::EFormat format, RHI::EImageLayout oldLayout, RHI::EImageLayout newLayout)
{
	cmd->m_vulkan.m_commandBuffer->ImageMemoryBarrier(image->m_vulkan.m_image, (VkFormat)format, (VkImageLayout)oldLayout, (VkImageLayout)newLayout);
}

void VulkanGraphicsDriver::BlitImage(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr src, RHI::RHITexturePtr dst, glm::ivec4 srcRegionRect, glm::ivec4 dstRegionRect, RHI::ETextureFiltration filtration)
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

	cmd->m_vulkan.m_commandBuffer->BlitImage(src->m_vulkan.m_image, dst->m_vulkan.m_image, srcRect, dstRect, (VkFilter)filtration);
}

void VulkanGraphicsDriver::ClearImage(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr dst, const glm::vec4& clearColor)
{
	cmd->m_vulkan.m_commandBuffer->ClearImage(dst->m_vulkan.m_image, clearColor);
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
		clearValue.color = { clearColor.x, clearColor.y, clearColor.z, clearColor.w };
		clearValue.depthStencil = VulkanApi::DefaultClearDepthStencilValue;

		auto vulkanRenderer = App::GetSubmodule<RHI::Renderer>()->GetDriver().DynamicCast<VulkanGraphicsDriver>();
		VulkanImageViewPtr vulkanDepthStencil = depthStencilAttachment->m_vulkan.m_imageView;

		const auto depthExtents = glm::ivec2(vulkanDepthStencil->GetImage()->m_extent.width, vulkanDepthStencil->GetImage()->m_extent.height);
		VulkanImageViewPtr msaaDepthStencilTarget = vulkanRenderer->GetOrAddMsaaRenderTarget((RHI::ETextureFormat)vulkanDepthStencil->GetImage()->m_format, depthExtents)->m_vulkan.m_imageView;

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
	clearValue.color = { clearColor.x, clearColor.y, clearColor.z, clearColor.w };
	clearValue.depthStencil = VulkanApi::DefaultClearDepthStencilValue;

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
	clearValue.color = { clearColor.x, clearColor.y, clearColor.z, clearColor.w };
	clearValue.depthStencil = VulkanApi::DefaultClearDepthStencilValue;

	cmd->m_vulkan.m_commandBuffer->BeginRenderPassEx(attachments,
		depthStencilAttachment->m_vulkan.m_imageView,
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
			renderArea, offset, bClearRenderTargets, clearColor, false);
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
		clearValue.color = { clearColor.x, clearColor.y, clearColor.z, clearColor.w };
		clearValue.depthStencil = VulkanApi::DefaultClearDepthStencilValue;

		auto vulkanRenderer = App::GetSubmodule<RHI::Renderer>()->GetDriver().DynamicCast<VulkanGraphicsDriver>();
		VulkanImageViewPtr vulkanDepthStencil = depthStencilAttachment->m_vulkan.m_imageView;

		const auto depthExtents = glm::ivec2(vulkanDepthStencil->GetImage()->m_extent.width, vulkanDepthStencil->GetImage()->m_extent.height);
		VulkanImageViewPtr msaaDepthStencilTarget = vulkanRenderer->GetOrAddMsaaRenderTarget((RHI::ETextureFormat)vulkanDepthStencil->GetImage()->m_format, depthExtents)->m_vulkan.m_imageView;

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

void VulkanGraphicsDriver::BeginSecondaryCommandList(RHI::RHICommandListPtr cmd, bool bOneTimeSubmit, bool bSupportMultisampling)
{
	uint32_t flags = bOneTimeSubmit ? VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT : 0;

	assert(cmd->m_vulkan.m_commandBuffer->GetLevel() == VK_COMMAND_BUFFER_LEVEL_SECONDARY);

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

	cmd->m_vulkan.m_commandBuffer->BeginSecondaryCommandList(TVector<VkFormat>{device->GetColorFormat()}, device->GetDepthFormat(), flags, VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT, bSupportMultisampling);
}

void VulkanGraphicsDriver::BeginCommandList(RHI::RHICommandListPtr cmd, bool bOneTimeSubmit)
{
	assert(cmd->m_vulkan.m_commandBuffer->GetLevel() != VK_COMMAND_BUFFER_LEVEL_SECONDARY);

	uint32_t flags = bOneTimeSubmit ? VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT : 0;
	cmd->m_vulkan.m_commandBuffer->BeginCommandList(flags);
}

void VulkanGraphicsDriver::EndCommandList(RHI::RHICommandListPtr cmd)
{
	cmd->m_vulkan.m_commandBuffer->EndCommandList();
}

void VulkanGraphicsDriver::UpdateShaderBinding(RHI::RHICommandListPtr cmd, RHI::RHIShaderBindingPtr parameter, const void* pData, size_t size, size_t variableOffset)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	auto& binding = parameter->m_vulkan.m_valueBinding->Get();
	Update(cmd, *binding, pData, size, variableOffset);
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
	size_t m_bufferOffset;
	size_t m_memoryOffset;

#if defined(SAILOR_VULKAN_COMBINE_STAGING_BUFFERS)
	SAILOR_PROFILE_BLOCK("Allocate space in staging buffer allocator");
	auto stagingBufferManagedPtr = device->GetStagingBufferAllocator()->Allocate(size, requirements.alignment);
	SAILOR_PROFILE_END_BLOCK();
	m_bufferOffset = (*stagingBufferManagedPtr).m_offset;
	m_memoryOffset = (**stagingBufferManagedPtr).m_offset;
	stagingBuffer = (*stagingBufferManagedPtr).m_buffer;
	cmd->m_vulkan.m_commandBuffer->AddDependency(stagingBufferManagedPtr, device->GetStagingBufferAllocator());
#elif defined(SAILOR_VULKAN_SHARE_DEVICE_MEMORY_FOR_STAGING_BUFFERS)

	auto& stagingMemoryAllocator = device->GetMemoryAllocator((VkMemoryPropertyFlags)(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT), requirements);

	SAILOR_PROFILE_BLOCK("Allocate staging memory");
	auto data = stagingMemoryAllocator.Allocate(size, requirements.alignment);
	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Create staging buffer");
	stagingBuffer = VulkanBufferPtr::Make(device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_CONCURRENT);
	stagingBuffer->Compile();
	VK_CHECK(stagingBuffer->Bind(data));
	SAILOR_PROFILE_END_BLOCK();

	m_bufferOffset = 0;
	m_memoryOffset = (*data).m_offset;
#else

	SAILOR_PROFILE_BLOCK("Allocate staging device memory");
	auto deviceMemoryPtr = VulkanDeviceMemoryPtr::Make(device, device->GetMemoryRequirements_StagingBuffer(), VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Create staging buffer");
	stagingBuffer = VulkanBufferPtr::Make(device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_CONCURRENT);
	stagingBuffer->Compile();
	VK_CHECK(stagingBuffer->Bind(deviceMemoryPtr, 0));
	SAILOR_PROFILE_END_BLOCK();

	m_bufferOffset = 0;
	m_memoryOffset = 0;
#endif

	SAILOR_PROFILE_BLOCK("Copy data to staging buffer");
	stagingBuffer->GetMemoryDevice()->Copy(m_memoryOffset, size, data);
	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Copy from staging to video ram command");
	cmd->m_vulkan.m_commandBuffer->CopyBuffer(*stagingBufferManagedPtr, bufferPtr, size, 0, offset);
	SAILOR_PROFILE_END_BLOCK();
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
	assert(shaderBinding->IsBind());

	RHI::ShaderLayoutBindingMember bindingLayout;
	if (!shaderBinding->FindVariableInUniformBuffer(variable, bindingLayout))
	{
		assert(false);
	}

	UpdateShaderBinding(cmd, shaderBinding, value, size, bindingLayout.m_absoluteOffset);
}

void VulkanGraphicsDriver::UpdateMesh(RHI::RHIMeshPtr mesh, const TVector<RHI::VertexP3N3UV2C4>& vertices, const TVector<uint32_t>& indices)
{
	auto device = m_vkInstance->GetMainDevice();

	const VkDeviceSize bufferSize = sizeof(vertices[0]) * vertices.Num();
	const VkDeviceSize indexBufferSize = sizeof(indices[0]) * indices.Num();

	RHI::RHICommandListPtr cmdList = CreateCommandList(false, true);
	RHI::Renderer::GetDriverCommands()->BeginCommandList(cmdList, true);

	const RHI::EBufferUsageFlags flags = RHI::EBufferUsageBit::VertexBuffer_Bit | RHI::EBufferUsageBit::IndexBuffer_Bit;

#if defined(SAILOR_VULKAN_STORE_VERTICES_INDICES_IN_SSBO)
	auto& ssboAllocator = GetMeshSsboAllocator();

	mesh->m_vertexBuffer = RHI::RHIBufferPtr::Make(flags);
	mesh->m_indexBuffer = RHI::RHIBufferPtr::Make(flags);

	const auto& vertexDescription = GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();

	mesh->m_vertexBuffer->m_vulkan.m_buffer = ssboAllocator->Allocate(bufferSize, vertexDescription->GetVertexStride());
	mesh->m_vertexBuffer->m_vulkan.m_bufferAllocator = ssboAllocator;

	mesh->m_indexBuffer->m_vulkan.m_buffer = ssboAllocator->Allocate(indexBufferSize, device->GetMinSsboOffsetAlignment());
	mesh->m_indexBuffer->m_vulkan.m_bufferAllocator = ssboAllocator;

	UpdateBuffer(cmdList, mesh->m_vertexBuffer, &vertices[0], bufferSize);
	UpdateBuffer(cmdList, mesh->m_indexBuffer, &indices[0], indexBufferSize);
#else
	mesh->m_vertexBuffer = CreateBuffer(cmdList,
		&vertices[0],
		bufferSize,
		flags);

	mesh->m_indexBuffer = CreateBuffer(cmdList,
		&indices[0],
		indexBufferSize,
		flags);
#endif

	RHI::Renderer::GetDriverCommands()->EndCommandList(cmdList);

	// Create fences to track the state of mesh creation
	RHI::RHIFencePtr fence = RHI::RHIFencePtr::Make();
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
	void* pPushConstantsData,
	uint32_t sizePushConstantsData)
{
	assert(computeShader->GetStage() == RHI::EShaderStage::Compute);

	if (computeShader->GetStage() != RHI::EShaderStage::Compute)
	{
		return;
	}

	VulkanComputePipelinePtr computePipeline = GetOrAddComputePipeline(computeShader);
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
	cmd->m_vulkan.m_commandBuffer->BindPipeline(material->m_vulkan.m_pipeline);
	if (material->GetRenderState().GetDepthBias() != 0.0f)
	{
		cmd->m_vulkan.m_commandBuffer->SetDepthBias(material->GetRenderState().GetDepthBias());
	}
}

void VulkanGraphicsDriver::BindVertexBuffer(RHI::RHICommandListPtr cmd, RHI::RHIBufferPtr vertexBuffer, uint32_t offset)
{
	auto ptr = *vertexBuffer->m_vulkan.m_buffer;
	TVector<VulkanBufferPtr> buffers{ ptr.m_buffer };

	//The offset is handled by RHI::RHIBuffer
	cmd->m_vulkan.m_commandBuffer->BindVertexBuffers(buffers, { offset });
}

void VulkanGraphicsDriver::BindIndexBuffer(RHI::RHICommandListPtr cmd, RHI::RHIBufferPtr indexBuffer, uint32_t offset)
{
	cmd->m_vulkan.m_commandBuffer->BindIndexBuffer(indexBuffer->m_vulkan.m_buffer.m_ptr.m_buffer, offset);
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
		m_cachedDescriptorSets.At_Lock(cache).m_second = 0;
		VulkanDescriptorSetPtr& cachedDescriptorSet = m_cachedDescriptorSets[cache].m_first;

		if (cachedDescriptorSet.IsValid())
		{
			descriptorSets.Add(cachedDescriptorSet);
			m_cachedDescriptorSets.Unlock(cache);
			continue;
		}

		SAILOR_PROFILE_BLOCK("Track changes");
		TVector<VulkanDescriptorPtr> descriptors;
		TVector<VkDescriptorSetLayoutBinding> descriptionSetLayouts;

		for (const auto& binding : shaderBindings[i]->GetShaderBindings())
		{
			if (binding.m_second->IsBind())
			{
				if (!materialLayout->m_descriptorSetLayoutBindings.ContainsIf(
					[&](const auto& lhs)
				{
					auto& layout = binding.m_second->GetLayout();
					return lhs.binding == layout.m_binding && lhs.descriptorType == (VkDescriptorType)layout.m_type;
				}))
				{
					// We don't add extra bindings 
					continue;
				}

				if (binding.m_second->GetTextureBinding())
				{
					auto& texture = binding.m_second->GetTextureBinding();

					if (binding.m_second->GetLayout().m_type == RHI::EShaderBindingType::CombinedImageSampler)
					{
						auto descr = VulkanDescriptorCombinedImagePtr::Make(binding.m_second->m_vulkan.m_descriptorSetLayout.binding, 0,
							device->GetSamplers()->GetSampler(texture->GetFiltration(), texture->GetClamping(), texture->ShouldGenerateMips()),
							texture->m_vulkan.m_imageView);
						descriptors.Add(descr);
					}
					else if (binding.m_second->GetLayout().m_type == RHI::EShaderBindingType::StorageImage)
					{
						auto descr = VulkanDescriptorStorageImagePtr::Make(binding.m_second->m_vulkan.m_descriptorSetLayout.binding, 0, texture->m_vulkan.m_imageView);
						descriptors.Add(descr);
					}

					descriptionSetLayouts.Add(binding.m_second->m_vulkan.m_descriptorSetLayout);
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
				}
			}
		}

		SAILOR_PROFILE_END_BLOCK();

		SAILOR_PROFILE_BLOCK("Create new descriptor sets");
		auto descriptorSet = VulkanDescriptorSetPtr::Make(device,
			device->GetCurrentThreadContext().m_descriptorPool,
			VulkanDescriptorSetLayoutPtr::Make(device, descriptionSetLayouts),
			descriptors);
		SAILOR_PROFILE_END_BLOCK();

		SAILOR_PROFILE_BLOCK("Compile new descriptor sets");
		descriptorSet->Compile();
		descriptorSets.Add(descriptorSet);

		cachedDescriptorSet = descriptorSets[i];
		m_cachedDescriptorSets.Unlock(cache);
		SAILOR_PROFILE_END_BLOCK();
	}

	return descriptorSets;
}

bool VulkanGraphicsDriver::CachedDescriptorSet::IsExpired() const
{
	return !m_layout.IsShared() || !m_binding.IsShared();
}

VulkanGraphicsDriver::CachedDescriptorSet& VulkanGraphicsDriver::CachedDescriptorSet::operator=(const CachedDescriptorSet& rhs)
{
	m_layout = rhs.m_layout;
	m_binding = rhs.m_binding;

	return *this;
}

bool VulkanGraphicsDriver::CachedDescriptorSet::operator==(const CachedDescriptorSet& rhs) const
{
	return m_layout == rhs.m_layout && m_binding == rhs.m_binding;
}

size_t VulkanGraphicsDriver::CachedDescriptorSet::GetHash() const
{
	size_t hash = m_layout.GetHash();
	HashCombine(hash, m_binding.GetHash());
	return hash;
}

VulkanGraphicsDriver::CachedDescriptorSet::CachedDescriptorSet(const VulkanPipelineLayoutPtr& material, const RHI::RHIShaderBindingSetPtr& binding) noexcept :
	m_layout(material),
	m_binding(binding)
{
}

void VulkanGraphicsDriver::BindShaderBindings(RHI::RHICommandListPtr cmd, RHI::RHIMaterialPtr material, const TVector<RHI::RHIShaderBindingSetPtr>& bindings)
{
	const TVector<VulkanDescriptorSetPtr>& sets = GetCompatibleDescriptorSets(material->m_vulkan.m_pipeline->m_layout, bindings);
	cmd->m_vulkan.m_commandBuffer->BindDescriptorSet(material->m_vulkan.m_pipeline->m_layout, sets, VK_PIPELINE_BIND_POINT_GRAPHICS);

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