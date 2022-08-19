#include "VulkanGraphicsDriver.h"
#include "RHI/Texture.h"
#include "RHI/Surface.h"
#include "RHI/Fence.h"
#include "RHI/Mesh.h"
#include "RHI/Buffer.h"
#include "RHI/Material.h"
#include "RHI/Shader.h"
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
	m_backBuffer.Clear();
	m_depthStencilBuffer.Clear();
	m_cachedMsaaRenderTargets.Clear();

	TrackResources_ThreadSafe();

	m_trackedFences.Clear();
	m_uniformBuffers.Clear();
	m_storageBuffer.Clear();

	// Waiting finishing releasing of rendering resources
	App::GetSubmodule<Tasks::Scheduler>()->WaitIdle(Tasks::EThreadType::Render);
	App::GetSubmodule<Tasks::Scheduler>()->WaitIdle(Tasks::EThreadType::RHI);

	GraphicsDriver::Vulkan::VulkanApi::Shutdown();
}

bool VulkanGraphicsDriver::ShouldFixLostDevice(const Win32::Window* pViewport)
{
	return m_vkInstance->GetMainDevice()->ShouldFixLostDevice(pViewport);
}

void VulkanGraphicsDriver::FixLostDevice(const Win32::Window* pViewport)
{
	SAILOR_PROFILE_FUNCTION();

	if (m_vkInstance->GetMainDevice()->ShouldFixLostDevice(pViewport))
	{
		SAILOR_PROFILE_BLOCK("Fix lost device");

		m_vkInstance->WaitIdle();
		m_vkInstance->GetMainDevice()->FixLostDevice(pViewport);

		SAILOR_PROFILE_END_BLOCK();
	}
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
	const TVector<RHI::RHICommandListPtr>* primaryCommandBuffers,
	const TVector<RHI::RHICommandListPtr>* secondaryCommandBuffers,
	TVector<RHI::RHISemaphorePtr> waitSemaphores) const
{
	TVector<VulkanCommandBufferPtr> primaryBuffers;
	TVector<VulkanCommandBufferPtr> secondaryBuffers;
	TVector<VulkanSemaphorePtr> vkWaitSemaphores;

	if (primaryCommandBuffers != nullptr)
	{
		for (auto& buffer : *primaryCommandBuffers)
		{
			primaryBuffers.Add(buffer->m_vulkan.m_commandBuffer);
		}
	}

	if (secondaryCommandBuffers != nullptr)
	{
		for (auto& buffer : *secondaryCommandBuffers)
		{
			secondaryBuffers.Add(buffer->m_vulkan.m_commandBuffer);
		}
	}

	if (waitSemaphores.Num() > 0)
	{
		for (auto& buffer : waitSemaphores)
		{
			vkWaitSemaphores.Add(buffer->m_vulkan.m_semaphore);
		}
	}

	return m_vkInstance->GetMainDevice()->PresentFrame(state, std::move(primaryBuffers), std::move(secondaryBuffers), std::move(vkWaitSemaphores));
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

RHI::RHIBufferPtr VulkanGraphicsDriver::CreateBuffer(size_t size, RHI::EBufferUsageFlags usage)
{
	SAILOR_PROFILE_FUNCTION();

	RHI::RHIBufferPtr res = RHI::RHIBufferPtr::Make(usage);
	res->m_vulkan.m_buffer = m_vkInstance->CreateBuffer(m_vkInstance->GetMainDevice(), size, (uint16_t)usage, VkMemoryPropertyFlagBits::VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	return res;
}

RHI::RHIBufferPtr VulkanGraphicsDriver::CreateBuffer(RHI::RHICommandListPtr& cmdList, const void* pData, size_t size, RHI::EBufferUsageFlags usage)
{
	SAILOR_PROFILE_FUNCTION();

	RHI::RHIBufferPtr outBuffer = CreateBuffer(size, usage);

	outBuffer->m_vulkan.m_buffer = m_vkInstance->CreateBuffer(cmdList->m_vulkan.m_commandBuffer,
		m_vkInstance->GetMainDevice(),
		pData, size, (uint16_t)usage);

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
	res->m_vulkan.m_buffer = m_vkInstance->CreateBuffer_Immediate(m_vkInstance->GetMainDevice(), pData, size, (uint32_t)usage);
	return res;
}

void VulkanGraphicsDriver::CopyBuffer_Immediate(RHI::RHIBufferPtr src, RHI::RHIBufferPtr dst, size_t size)
{
	m_vkInstance->CopyBuffer_Immediate(m_vkInstance->GetMainDevice(), src->m_vulkan.m_buffer, dst->m_vulkan.m_buffer, size);
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

	auto device = m_vkInstance->GetMainDevice();
	RHI::RHITexturePtr outTexture = RHI::RHITexturePtr::Make(filtration, clamping, false, RHI::EImageLayout::ColorAttachmentOptimal);

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
		(VkImageLayout)RHI::EImageLayout::ColorAttachmentOptimal);

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
		RHI::EImageLayout::ColorAttachmentOptimal);
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

	RHI::RHITexturePtr resolved = CreateRenderTarget(extent, mipLevels, type, format, filtration, clamping, usage);
	RHI::RHITexturePtr target = resolved;

	bool bNeedsResolved = m_vkInstance->GetMainDevice()->GetCurrentMsaaSamples() != VK_SAMPLE_COUNT_1_BIT;
	if (bNeedsResolved)
	{
		target = RHI::RHITexturePtr::Make(filtration, clamping, false, RHI::EImageLayout::ColorAttachmentOptimal);

		VkExtent3D vkExtent;
		vkExtent.width = extent.x;
		vkExtent.height = extent.y;
		vkExtent.depth = extent.z;

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
				auto descr = VulkanDescriptorImagePtr::Make(binding.m_second->m_vulkan.m_descriptorSetLayout.binding, 0,
					device->GetSamplers()->GetSampler(texture->GetFiltration(), texture->GetClamping(), texture->ShouldGenerateMips()),
					texture->m_vulkan.m_imageView);

				descriptors.Add(descr);
				descriptionSetLayouts.Add(binding.m_second->m_vulkan.m_descriptorSetLayout);
			}
			else if (binding.m_second->m_vulkan.m_valueBinding)
			{
				auto& valueBinding = (*binding.m_second->m_vulkan.m_valueBinding);
				auto descr = VulkanDescriptorBufferPtr::Make(binding.m_second->m_vulkan.m_descriptorSetLayout.binding, 0,
					valueBinding.m_buffer, valueBinding.m_offset, valueBinding.m_size, binding.m_second->GetLayout().m_type);

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
		if (layoutBinding.m_set == 0 || layoutBinding.m_set == 1)
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
		auto& binding = shaderBindings->GetOrCreateShaderBinding(layoutBinding.m_name);

		if (layoutBinding.m_type == RHI::EShaderBindingType::UniformBuffer)
		{
			auto& uniformAllocator = GetUniformBufferAllocator(layoutBinding.m_name);
			binding->m_vulkan.m_valueBinding = uniformAllocator->Allocate(layoutBinding.m_size, device->GetUboOffsetAlignment(layoutBinding.m_size));
			binding->m_vulkan.m_bufferAllocator = uniformAllocator;
			binding->m_vulkan.m_descriptorSetLayout = vkLayoutBinding;
			binding->SetLayout(layoutBinding);
		}
		else if (layoutBinding.m_type == RHI::EShaderBindingType::StorageBuffer)
		{
			auto& storageAllocator = GetStorageBufferAllocator();
			binding->m_vulkan.m_valueBinding = storageAllocator->Allocate(layoutBinding.m_size, 0 /*device->GetSsboOffsetAlignment(layoutBinding.m_size)*/);
			binding->m_vulkan.m_bufferAllocator = storageAllocator;
			binding->m_vulkan.m_descriptorSetLayout = vkLayoutBinding;
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
		vkPushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

		pushConstants.Emplace(vkPushConstant);
	}

	// TODO: Rearrange descriptorSetLayouts to support vector of descriptor sets
	auto pipelineLayout = VulkanPipelineLayoutPtr::Make(device,
		descriptorSetLayouts,
		pushConstants,
		0);

	res->m_vulkan.m_pipeline = VulkanPipelinePtr::Make(device,
		pipelineLayout,
		TVector{ vertex->m_vulkan.m_shader, fragment->m_vulkan.m_shader },
		device->GetPipelineBuilder()->BuildPipeline(vertexDescription, topology, renderState),
		0);

	res->m_vulkan.m_pipeline->m_renderPass = device->GetRenderPass();
	res->m_vulkan.m_pipeline->Compile();
	res->SetBindings(shaderBindigs);

	return res;
}

TSharedPtr<VulkanBufferAllocator>& VulkanGraphicsDriver::GetStorageBufferAllocator()
{
	if (!m_storageBuffer)
	{
		const size_t StorageBufferBlockSize = 16 * 1024 * 1024u;
		const size_t ReservedSize = 8 * 1024 * 1024u;

		m_storageBuffer = TSharedPtr<VulkanBufferAllocator>::Make(StorageBufferBlockSize, 1024 * 1024u, StorageBufferBlockSize);
		m_storageBuffer->GetGlobalAllocator().SetUsage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
		m_storageBuffer->GetGlobalAllocator().SetMemoryProperties(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	}
	return m_storageBuffer;
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

	auto& shaderBinding = bindings->GetOrCreateShaderBinding(parameter);
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

RHI::RHIShaderBindingPtr VulkanGraphicsDriver::AddSsboToShaderBindings(RHI::RHIShaderBindingSetPtr& pShaderBindings, const std::string& name, size_t elementSize, size_t numElements, uint32_t shaderBinding)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	RHI::RHIShaderBindingPtr binding = pShaderBindings->GetOrCreateShaderBinding(name);

	TSharedPtr<VulkanBufferAllocator> allocator = GetStorageBufferAllocator();
	binding->m_vulkan.m_valueBinding = allocator->Allocate(elementSize * numElements, 0 /*device->GetSsboOffsetAlignment(elementSize)*/);
	binding->m_vulkan.m_storageInstanceIndex = (uint32_t)((**binding->m_vulkan.m_valueBinding).m_offset / elementSize);

	binding->m_vulkan.m_bufferAllocator = allocator;

	auto valueBinding = *(binding->m_vulkan.m_valueBinding);

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

	RHI::RHIShaderBindingPtr binding = pShaderBindings->GetOrCreateShaderBinding(name);
	TSharedPtr<VulkanBufferAllocator> allocator;

	if (const bool bIsStorage = bufferType == RHI::EShaderBindingType::StorageBuffer)
	{
		allocator = GetStorageBufferAllocator();
		binding->m_vulkan.m_valueBinding = allocator->Allocate(size, 0);
		binding->m_vulkan.m_storageInstanceIndex = (uint32_t)((**binding->m_vulkan.m_valueBinding).m_offset / size);
	}
	else
	{
		allocator = GetUniformBufferAllocator(name);
		binding->m_vulkan.m_valueBinding = allocator->Allocate(size, device->GetUboOffsetAlignment(size));
	}

	binding->m_vulkan.m_bufferAllocator = allocator;

	auto valueBinding = *(binding->m_vulkan.m_valueBinding);

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
	RHI::RHIShaderBindingPtr binding = pShaderBindings->GetOrCreateShaderBinding(name);

	RHI::ShaderLayoutBinding layout;
	layout.m_binding = shaderBinding;
	layout.m_name = name;
	layout.m_type = RHI::EShaderBindingType::CombinedImageSampler;

	binding->SetLayout(layout);
	binding->m_vulkan.m_descriptorSetLayout = VulkanApi::CreateDescriptorSetLayoutBinding(layout.m_binding, (VkDescriptorType)layout.m_type);
	binding->SetTextureBinding(texture);

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

			auto descriptor = (*descrIt).DynamicCast<VulkanDescriptorImage>();
			descriptor->SetImageView(value->m_vulkan.m_imageView);
			bindings->m_vulkan.m_descriptorSet->Compile();

			return;
		}
		else
		{
			// Add new texture binding
			auto textureBinding = bindings->GetOrCreateShaderBinding(parameter);
			textureBinding->SetTextureBinding(value);
			textureBinding->m_vulkan.m_descriptorSetLayout = VulkanApi::CreateDescriptorSetLayoutBinding(layoutBindings[index].m_binding, (VkDescriptorType)layoutBindings[index].m_type);
			UpdateDescriptorSet(bindings);

			return;
		}
	}

	SAILOR_LOG("Trying to update not bind uniform sampler");
}

RHI::RHITexturePtr VulkanGraphicsDriver::GetOrCreateMsaaRenderTarget(RHI::EFormat textureFormat, glm::ivec2 extent)
{
	// TODO: Handle the extents too
	size_t hash = (size_t)textureFormat;
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

void VulkanGraphicsDriver::ExecuteSecondaryCommandList(RHI::RHICommandListPtr cmd, RHI::RHICommandListPtr cmdSecondary)
{
	//TODO: Check secondary
	cmd->m_vulkan.m_commandBuffer->Execute(cmdSecondary->m_vulkan.m_commandBuffer);
}

void VulkanGraphicsDriver::RenderSecondaryCommandBuffers(RHI::RHICommandListPtr cmd,
	TVector<RHI::RHICommandListPtr> secondaryCmds,
	const TVector<RHI::RHITexturePtr>& colorAttachments,
	RHI::RHITexturePtr depthStencilAttachment,
	glm::ivec4 renderArea,
	glm::ivec2 offset,
	bool bClearRenderTargets,
	glm::vec4 clearColor,
	bool bSupportMultisampling)
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

	cmd->m_vulkan.m_commandBuffer->BeginRenderPassEx(attachments,
		depthStencilAttachment->m_vulkan.m_imageView,
		rect,
		VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT_KHR,
		VkOffset2D{ .x = offset.x, .y = offset.y },
		bSupportMultisampling,
		bClearRenderTargets);

	for (auto& el : secondaryCmds)
	{
		ExecuteSecondaryCommandList(cmd, el);
	}

	EndRenderPass(cmd);
}

void VulkanGraphicsDriver::BeginRenderPass(RHI::RHICommandListPtr cmd, const TVector<RHI::RHITexturePtr>& colorAttachments,
	RHI::RHITexturePtr depthStencilAttachment,
	glm::ivec4 renderArea,
	glm::ivec2 offset,
	bool bClearRenderTargets,
	glm::vec4 clearColor,
	bool bSupportMultisampling)
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

	cmd->m_vulkan.m_commandBuffer->BeginRenderPassEx(attachments,
		depthStencilAttachment->m_vulkan.m_imageView,
		rect,
		0,
		VkOffset2D{ .x = offset.x, .y = offset.y },
		bSupportMultisampling,
		bClearRenderTargets);
}

void VulkanGraphicsDriver::BeginRenderPass(RHI::RHICommandListPtr cmd,
	const TVector<RHI::RHISurfacePtr>& colorAttachments,
	RHI::RHISurfacePtr depthStencilAttachment,
	glm::ivec4 renderArea,
	glm::ivec2 offset,
	bool bClearRenderTargets,
	glm::vec4 clearColor)
{
	const bool bNeedsResolve = depthStencilAttachment->NeedsResolve() || (colorAttachments.Num() > 0 && colorAttachments[0]->NeedsResolve());

	TVector<RHI::RHITexturePtr> atts;
	atts.Reserve(colorAttachments.Num());
	for (uint32_t i = 0; i < colorAttachments.Num(); i++)
	{
		atts.Add(colorAttachments[i]->GetResolved());
	}

	if (!bNeedsResolve)
	{
		BeginRenderPass(cmd, atts, depthStencilAttachment->GetResolved(),
			renderArea, offset, bClearRenderTargets, clearColor, false);

		return;
	}

	// TODO: Use surface target as msaa target
	assert(0);
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

	auto& binding = parameter->m_vulkan.m_valueBinding;
	auto dstBuffer = binding.m_ptr.m_buffer;

	Update(cmd, dstBuffer, pData, size, binding.m_offset + binding.m_alignmentOffset + variableOffset);
}

void VulkanGraphicsDriver::UpdateBuffer(RHI::RHICommandListPtr cmd, RHI::RHIBufferPtr buffer, const void* pData, size_t size, size_t offset)
{
	Update(cmd, buffer->m_vulkan.m_buffer, pData, size, offset);
}

void VulkanGraphicsDriver::Update(RHI::RHICommandListPtr cmd, VulkanBufferPtr dstBuffer, const void* data, size_t size, size_t offset)
{
	SAILOR_PROFILE_FUNCTION();

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
	cmd->m_vulkan.m_commandBuffer->CopyBuffer(stagingBuffer, dstBuffer, size, m_bufferOffset, offset);
	SAILOR_PROFILE_END_BLOCK();
}

void VulkanGraphicsDriver::SetMaterialParameter(RHI::RHICommandListPtr cmd, RHI::RHIShaderBindingSetPtr bindings, const std::string& binding, const std::string& variable, const void* value, size_t size)
{
	SAILOR_PROFILE_FUNCTION();

	if (bindings->HasBinding(binding))
	{
		auto device = m_vkInstance->GetMainDevice();
		auto& shaderBinding = bindings->GetOrCreateShaderBinding(binding);
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

void VulkanGraphicsDriver::BindMaterial(RHI::RHICommandListPtr cmd, RHI::RHIMaterialPtr material)
{
	cmd->m_vulkan.m_commandBuffer->BindPipeline(material->m_vulkan.m_pipeline);
	if (material->GetRenderState().GetDepthBias() != 0.0f)
	{
		cmd->m_vulkan.m_commandBuffer->SetDepthBias(material->GetRenderState().GetDepthBias());
	}
}

void VulkanGraphicsDriver::BindVertexBuffers(RHI::RHICommandListPtr cmd, RHI::RHIBufferPtr vertexBuffer)
{
	cmd->m_vulkan.m_commandBuffer->BindVertexBuffers({ vertexBuffer->m_vulkan.m_buffer });
}

void VulkanGraphicsDriver::BindIndexBuffer(RHI::RHICommandListPtr cmd, RHI::RHIBufferPtr indexBuffer)
{
	cmd->m_vulkan.m_commandBuffer->BindIndexBuffer(indexBuffer->m_vulkan.m_buffer);
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

void VulkanGraphicsDriver::BindShaderBindings(RHI::RHICommandListPtr cmd, RHI::RHIMaterialPtr material, const TVector<RHI::RHIShaderBindingSetPtr>& bindings)
{
	TVector<VulkanDescriptorSetPtr> sets;
	for (auto& binding : bindings)
	{
		sets.Add(binding->m_vulkan.m_descriptorSet);
	}

	cmd->m_vulkan.m_commandBuffer->BindDescriptorSet(material->m_vulkan.m_pipeline->m_layout, sets);
}

void VulkanGraphicsDriver::DrawIndexed(RHI::RHICommandListPtr cmd, RHI::RHIBufferPtr indexBuffer, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, uint32_t vertexOffset, uint32_t firstInstance)
{
	cmd->m_vulkan.m_commandBuffer->DrawIndexed(indexBuffer->m_vulkan.m_buffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
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