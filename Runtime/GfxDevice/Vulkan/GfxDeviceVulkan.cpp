#include "GfxDeviceVulkan.h"
#include "RHI/Texture.h"
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
#include "VulkanShaderModule.h"
#include "VulkanBufferMemory.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"

using namespace Sailor;
using namespace Sailor::GfxDevice::Vulkan;

void GfxDeviceVulkan::Initialize(const Win32::Window* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug)
{
	GfxDevice::Vulkan::VulkanApi::Initialize(pViewport, msaaSamples, bIsDebug);
	m_vkInstance = GfxDevice::Vulkan::VulkanApi::GetInstance();
}

GfxDeviceVulkan::~GfxDeviceVulkan()
{
	TrackResources_ThreadSafe();

	m_trackedFences.clear();
	m_uniformBuffers.clear();
	m_storageBuffer.Clear();

	// Waiting finishing releasing of rendering resources
	App::GetSubmodule<JobSystem::Scheduler>()->ProcessJobsOnMainThread();
	App::GetSubmodule<JobSystem::Scheduler>()->WaitIdle(JobSystem::EThreadType::Rendering);
	App::GetSubmodule<JobSystem::Scheduler>()->WaitIdle(JobSystem::EThreadType::Worker);

	GfxDevice::Vulkan::VulkanApi::Shutdown();
}

bool GfxDeviceVulkan::ShouldFixLostDevice(const Win32::Window* pViewport)
{
	return m_vkInstance->GetMainDevice()->ShouldFixLostDevice(pViewport);
}

void GfxDeviceVulkan::FixLostDevice(const Win32::Window* pViewport)
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

bool GfxDeviceVulkan::PresentFrame(const class FrameState& state,
	const std::vector<RHI::CommandListPtr>* primaryCommandBuffers,
	const std::vector<RHI::CommandListPtr>* secondaryCommandBuffers,
	std::vector<RHI::SemaphorePtr> waitSemaphores) const
{
	std::vector<VulkanCommandBufferPtr> primaryBuffers;
	std::vector<VulkanCommandBufferPtr> secondaryBuffers;
	std::vector<VulkanSemaphorePtr> vkWaitSemaphores;

	if (primaryCommandBuffers != nullptr)
	{
		for (auto& buffer : *primaryCommandBuffers)
		{
			primaryBuffers.push_back(buffer->m_vulkan.m_commandBuffer);
		}
	}

	if (secondaryCommandBuffers != nullptr)
	{
		for (auto& buffer : *secondaryCommandBuffers)
		{
			secondaryBuffers.push_back(buffer->m_vulkan.m_commandBuffer);
		}
	}

	if (waitSemaphores.size() > 0)
	{
		for (auto& buffer : waitSemaphores)
		{
			vkWaitSemaphores.push_back(buffer->m_vulkan.m_semaphore);
		}
	}

	return m_vkInstance->GetMainDevice()->PresentFrame(state, std::move(primaryBuffers), std::move(secondaryBuffers), std::move(vkWaitSemaphores));
}

void GfxDeviceVulkan::WaitIdle()
{
	SAILOR_PROFILE_FUNCTION();

	m_vkInstance->WaitIdle();
}

void GfxDeviceVulkan::SubmitCommandList(RHI::CommandListPtr commandList, RHI::FencePtr fence, RHI::SemaphorePtr signalSemaphore, RHI::SemaphorePtr waitSemaphore)
{
	SAILOR_PROFILE_FUNCTION();

	SAILOR_PROFILE_BLOCK("Check fence");

	//if we have fence and that is null we should create device resource
	if (fence && !fence->m_vulkan.m_fence)
	{
		fence->m_vulkan.m_fence = VulkanFencePtr::Make(m_vkInstance->GetMainDevice());
	}

	SAILOR_PROFILE_END_BLOCK();

	std::vector<VulkanSemaphorePtr> signal;
	std::vector<VulkanSemaphorePtr> wait;

	SAILOR_PROFILE_BLOCK("Add semaphore dependencies");

	if (signalSemaphore)
	{
		commandList->m_vulkan.m_commandBuffer->AddDependency(signalSemaphore->m_vulkan.m_semaphore);
		signal.push_back(signalSemaphore->m_vulkan.m_semaphore);
	}

	if (waitSemaphore)
	{
		commandList->m_vulkan.m_commandBuffer->AddDependency(waitSemaphore->m_vulkan.m_semaphore);
		wait.push_back(waitSemaphore->m_vulkan.m_semaphore);
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

RHI::SemaphorePtr GfxDeviceVulkan::CreateWaitSemaphore()
{
	SAILOR_PROFILE_FUNCTION();
	auto device = m_vkInstance->GetMainDevice();

	RHI::SemaphorePtr res = RHI::SemaphorePtr::Make();
	res->m_vulkan.m_semaphore = VulkanSemaphorePtr::Make(device);
	return res;
}

RHI::CommandListPtr GfxDeviceVulkan::CreateCommandList(bool bIsSecondary, bool bOnlyTransferQueue)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	RHI::CommandListPtr cmdList = RHI::CommandListPtr::Make();
	cmdList->m_vulkan.m_commandBuffer = VulkanCommandBufferPtr::Make(device,
		bOnlyTransferQueue ? device->GetCurrentThreadContext().m_transferCommandPool : device->GetCurrentThreadContext().m_commandPool,
		bIsSecondary ? VK_COMMAND_BUFFER_LEVEL_SECONDARY : VK_COMMAND_BUFFER_LEVEL_PRIMARY);

	return cmdList;
}

RHI::BufferPtr GfxDeviceVulkan::CreateBuffer(size_t size, RHI::EBufferUsageFlags usage)
{
	SAILOR_PROFILE_FUNCTION();

	RHI::BufferPtr res = RHI::BufferPtr::Make(usage);
	res->m_vulkan.m_buffer = m_vkInstance->CreateBuffer(m_vkInstance->GetMainDevice(), size, (uint16_t)usage, VkMemoryPropertyFlagBits::VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	return res;
}

RHI::CommandListPtr GfxDeviceVulkan::CreateBuffer(RHI::BufferPtr& outBuffer, const void* pData, size_t size, RHI::EBufferUsageFlags usage)
{
	SAILOR_PROFILE_FUNCTION();

	outBuffer = CreateBuffer(size, usage);
	RHI::CommandListPtr cmdList = RHI::CommandListPtr::Make();

	cmdList->m_vulkan.m_commandBuffer = m_vkInstance->CreateBuffer(outBuffer->m_vulkan.m_buffer,
		m_vkInstance->GetMainDevice(),
		pData, size, (uint16_t)usage);

	return cmdList;
}

RHI::ShaderPtr GfxDeviceVulkan::CreateShader(RHI::EShaderStage shaderStage, const RHI::ShaderByteCode& shaderSpirv)
{
	SAILOR_PROFILE_FUNCTION();

	auto res = RHI::ShaderPtr::Make(shaderStage);
	res->m_vulkan.m_shader = VulkanShaderStagePtr::Make((VkShaderStageFlagBits)shaderStage, "main", m_vkInstance->GetMainDevice(), shaderSpirv);
	res->m_vulkan.m_shader->Compile();

	return res;
}

RHI::BufferPtr GfxDeviceVulkan::CreateBuffer_Immediate(const void* pData, size_t size, RHI::EBufferUsageFlags usage)
{
	SAILOR_PROFILE_FUNCTION();

	RHI::BufferPtr res = RHI::BufferPtr::Make(usage);
	res->m_vulkan.m_buffer = m_vkInstance->CreateBuffer_Immediate(m_vkInstance->GetMainDevice(), pData, size, (uint32_t)usage);
	return res;
}

void GfxDeviceVulkan::CopyBuffer_Immediate(RHI::BufferPtr src, RHI::BufferPtr dst, size_t size)
{
	m_vkInstance->CopyBuffer_Immediate(m_vkInstance->GetMainDevice(), src->m_vulkan.m_buffer, dst->m_vulkan.m_buffer, size);
}

RHI::TexturePtr GfxDeviceVulkan::CreateImage_Immediate(
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

	RHI::TexturePtr res = RHI::TexturePtr::Make(filtration, clamping, mipLevels > 1);
	res->m_vulkan.m_image = m_vkInstance->CreateImage_Immediate(m_vkInstance->GetMainDevice(), pData, size, vkExtent, mipLevels, (VkImageType)type, (VkFormat)format, VK_IMAGE_TILING_OPTIMAL, (uint32_t)usage);
	res->m_vulkan.m_imageView = VulkanImageViewPtr::Make(device, res->m_vulkan.m_image);
	res->m_vulkan.m_imageView->Compile();

	return res;
}

RHI::TexturePtr GfxDeviceVulkan::CreateImage(
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
	RHI::TexturePtr outTexture = RHI::TexturePtr::Make(filtration, clamping, mipLevels > 1);

	VkExtent3D vkExtent;
	vkExtent.width = extent.x;
	vkExtent.height = extent.y;
	vkExtent.depth = extent.z;

	RHI::CommandListPtr cmdList = RHI::Renderer::GetDriver()->CreateCommandList();
	cmdList->m_vulkan.m_commandBuffer = m_vkInstance->CreateImage(outTexture->m_vulkan.m_image, m_vkInstance->GetMainDevice(), pData, size, vkExtent, mipLevels, (VkImageType)type, (VkFormat)format, VK_IMAGE_TILING_OPTIMAL, (uint32_t)usage);

	outTexture->m_vulkan.m_imageView = VulkanImageViewPtr::Make(device, outTexture->m_vulkan.m_image);
	outTexture->m_vulkan.m_imageView->Compile();

	RHI::FencePtr fenceUpdateRes = RHI::FencePtr::Make();
	TrackDelayedInitialization(outTexture.GetRawPtr(), fenceUpdateRes);
	SubmitCommandList(cmdList, fenceUpdateRes);

	return outTexture;
}

void GfxDeviceVulkan::UpdateDescriptorSet(RHI::ShaderBindingSetPtr bindings)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();
	std::vector<VulkanDescriptorPtr> descriptors;
	std::vector<VkDescriptorSetLayoutBinding> descriptionSetLayouts;

	for (const auto& binding : bindings->GetShaderBindings())
	{
		if (binding.second->IsBind())
		{
			if (binding.second->GetTextureBinding())
			{
				auto& texture = binding.second->GetTextureBinding();
				auto descr = VulkanDescriptorImagePtr::Make(binding.second->m_vulkan.m_descriptorSetLayout.binding, 0,
					device->GetSamplers()->GetSampler(texture->GetFiltration(), texture->GetClamping(), texture->ShouldGenerateMips()),
					texture->m_vulkan.m_imageView);

				descriptors.push_back(descr);
				descriptionSetLayouts.push_back(binding.second->m_vulkan.m_descriptorSetLayout);
			}
			else if (binding.second->m_vulkan.m_valueBinding)
			{
				auto& valueBinding = (*binding.second->m_vulkan.m_valueBinding);
				auto descr = VulkanDescriptorBufferPtr::Make(binding.second->m_vulkan.m_descriptorSetLayout.binding, 0,
					valueBinding.m_buffer, valueBinding.m_offset, valueBinding.m_size, binding.second->GetLayout().m_type);

				descriptors.push_back(descr);
				descriptionSetLayouts.push_back(binding.second->m_vulkan.m_descriptorSetLayout);
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

RHI::MaterialPtr GfxDeviceVulkan::CreateMaterial(const RHI::RenderState& renderState, const Sailor::ShaderSetPtr& shader)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	std::vector<VulkanDescriptorSetLayoutPtr> descriptorSetLayouts;
	std::vector<RHI::ShaderLayoutBinding> bindings;

	// We need debug shaders to get full names from reflection
	VulkanApi::CreateDescriptorSetLayouts(device, { shader.Lock()->GetDebugVertexShaderRHI()->m_vulkan.m_shader, shader.Lock()->GetDebugFragmentShaderRHI()->m_vulkan.m_shader},
		descriptorSetLayouts, bindings);

#ifdef _DEBUG
	const bool bIsDebug = true;
#else
	const bool bIsDebug = false;
#endif

	auto vertex = shader.Lock()->GetVertexShaderRHI();
	auto fragment = shader.Lock()->GetFragmentShaderRHI();

	RHI::MaterialPtr res = RHI::MaterialPtr::Make(renderState, vertex, fragment);

	// TODO: Rearrange descriptorSetLayouts to support vector of descriptor sets
	auto pipelineLayout = VulkanPipelineLayoutPtr::Make(device,
		descriptorSetLayouts,
		std::vector<VkPushConstantRange>(),
		0);

	res->m_vulkan.m_pipeline = VulkanPipelinePtr::Make(device,
		pipelineLayout,
		std::vector{ vertex->m_vulkan.m_shader, fragment->m_vulkan.m_shader },
		device->GetPipelineBuilder()->BuildPipeline(renderState),
		0);

	res->m_vulkan.m_pipeline->m_renderPass = device->GetRenderPass();
	res->m_vulkan.m_pipeline->Compile();

	auto shaderBindings = CreateShaderBindings();
	res->SetBindings(shaderBindings);

	for (uint32_t i = 0; i < bindings.size(); i++)
	{
		auto& layoutBinding = bindings[i];
		if (layoutBinding.m_set == 0 || layoutBinding.m_set == 1)
		{
			// We skip 0 layout, it is frameData and would be binded in a different way
			// Also we skip 1 layout, that is per instance data and would be binded in a different way
			continue;
		}
		const auto& layoutBindings = descriptorSetLayouts[layoutBinding.m_set]->m_descriptorSetLayoutBindings;

		auto it = std::find_if(layoutBindings.begin(), layoutBindings.end(), [&layoutBinding](const auto& bind) { return bind.binding == layoutBinding.m_binding; });
		if (it == layoutBindings.end())
		{
			continue;
		}

		auto& vkLayoutBinding = *it;
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
			binding->m_vulkan.m_valueBinding = storageAllocator->Allocate(layoutBinding.m_size, 0);
			binding->m_vulkan.m_bufferAllocator = storageAllocator;
			binding->m_vulkan.m_descriptorSetLayout = vkLayoutBinding;
			binding->SetLayout(layoutBinding);
		}
	}

	shaderBindings->SetLayoutShaderBindings(bindings);
	UpdateDescriptorSet(shaderBindings);

	return res;
}

TSharedPtr<VulkanBufferAllocator>& GfxDeviceVulkan::GetStorageBufferAllocator()
{
	if (!m_storageBuffer)
	{
		const size_t StorageBufferBlockSize = 256 * 1024 * 1024u;
		const size_t ReservedSize = 64 * 1024 * 1024u;

		m_storageBuffer = TSharedPtr<VulkanBufferAllocator>::Make(StorageBufferBlockSize, 1024 * 1024u, StorageBufferBlockSize);
		m_storageBuffer->GetGlobalAllocator().SetUsage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
		m_storageBuffer->GetGlobalAllocator().SetMemoryProperties(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	}
	return m_storageBuffer;
}

TSharedPtr<VulkanBufferAllocator>& GfxDeviceVulkan::GetUniformBufferAllocator(const std::string& uniformTypeId)
{
	SAILOR_PROFILE_FUNCTION();

	auto it = m_uniformBuffers.find(uniformTypeId);
	if (it != m_uniformBuffers.end())
	{
		return (*it).second;
	}

	auto& uniformAllocator = m_uniformBuffers[uniformTypeId] = TSharedPtr<VulkanBufferAllocator>::Make();
	uniformAllocator->GetGlobalAllocator().SetUsage(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	uniformAllocator->GetGlobalAllocator().SetMemoryProperties(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	return uniformAllocator;
}

void GfxDeviceVulkan::UpdateShaderBinding_Immediate(RHI::ShaderBindingSetPtr bindings, const std::string& parameter, const void* value, size_t size)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	RHI::CommandListPtr commandList = RHI::CommandListPtr::Make();
	commandList->m_vulkan.m_commandBuffer = Vulkan::VulkanCommandBufferPtr::Make(device, device->GetCurrentThreadContext().m_commandPool, VkCommandBufferLevel::VK_COMMAND_BUFFER_LEVEL_SECONDARY);

	auto& shaderBinding = bindings->GetOrCreateShaderBinding(parameter);
	bool bShouldUpdateDescriptorSet = false;

	// All uniform buffers should be bound
	assert(shaderBinding->IsBind());

	UpdateShaderBinding(commandList, shaderBinding, value, size);

	SubmitCommandList_Immediate(commandList);
}

RHI::ShaderBindingSetPtr GfxDeviceVulkan::CreateShaderBindings()
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	RHI::ShaderBindingSetPtr res = RHI::ShaderBindingSetPtr::Make();
	return res;
}

void GfxDeviceVulkan::AddBufferToShaderBindings(RHI::ShaderBindingSetPtr& pShaderBindings, const std::string& name, size_t size, uint32_t shaderBinding, RHI::EShaderBindingType bufferType)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	RHI::ShaderBindingPtr binding = pShaderBindings->GetOrCreateShaderBinding(name);
	TSharedPtr<VulkanBufferAllocator> allocator;

	if (const bool bIsStorage = bufferType == RHI::EShaderBindingType::StorageBuffer)
	{
		allocator = GetStorageBufferAllocator();
		binding->m_vulkan.m_valueBinding = allocator->Allocate(size, device->GetSsboOffsetAlignment(size));
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
}

void GfxDeviceVulkan::AddSamplerToShaderBindings(RHI::ShaderBindingSetPtr& pShaderBindings, const std::string& name, RHI::TexturePtr texture, uint32_t shaderBinding)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();
	RHI::ShaderBindingPtr binding = pShaderBindings->GetOrCreateShaderBinding(name);

	RHI::ShaderLayoutBinding layout;
	layout.m_binding = shaderBinding;
	layout.m_name = name;
	layout.m_type = RHI::EShaderBindingType::CombinedImageSampler;

	binding->SetLayout(layout);
	binding->m_vulkan.m_descriptorSetLayout = VulkanApi::CreateDescriptorSetLayoutBinding(layout.m_binding, (VkDescriptorType)layout.m_type);
	binding->SetTextureBinding(texture);

	UpdateDescriptorSet(pShaderBindings);
}

void GfxDeviceVulkan::UpdateShaderBinding(RHI::ShaderBindingSetPtr bindings, const std::string& parameter, RHI::TexturePtr value)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();
	const auto& layoutBindings = bindings->GetLayoutBindings();

	auto it = std::find_if(layoutBindings.begin(), layoutBindings.end(), [&parameter](const RHI::ShaderLayoutBinding& shaderLayoutBinding)
		{
			return shaderLayoutBinding.m_name == parameter;
		});

	if (it != layoutBindings.end())
	{
		auto& descriptors = bindings->m_vulkan.m_descriptorSet->m_descriptors;
		auto descrIt = std::find_if(descriptors.begin(), descriptors.end(), [&it](const VulkanDescriptorPtr& descriptor)
			{
				return descriptor->GetBinding() == it->m_binding;
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
			textureBinding->m_vulkan.m_descriptorSetLayout = VulkanApi::CreateDescriptorSetLayoutBinding(it->m_binding, (VkDescriptorType)it->m_type);
			UpdateDescriptorSet(bindings);

			return;
		}
	}

	SAILOR_LOG("Trying to update not bind uniform sampler");
}

// IGfxDeviceCommands

void GfxDeviceVulkan::BeginCommandList(RHI::CommandListPtr cmd)
{
	cmd->m_vulkan.m_commandBuffer->BeginCommandList(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
}

void GfxDeviceVulkan::EndCommandList(RHI::CommandListPtr cmd)
{
	cmd->m_vulkan.m_commandBuffer->EndCommandList();
}

void GfxDeviceVulkan::UpdateShaderBinding(RHI::CommandListPtr cmd, RHI::ShaderBindingPtr parameter, const void* pData, size_t size, size_t variableOffset)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	auto& binding = parameter->m_vulkan.m_valueBinding;
	auto dstBuffer = binding.m_ptr.m_buffer;

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
	stagingBuffer->GetMemoryDevice()->Copy(m_memoryOffset, size, pData);
	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Copy from staging to video ram command");
	cmd->m_vulkan.m_commandBuffer->CopyBuffer(stagingBuffer, dstBuffer, size, m_bufferOffset, binding.m_offset + variableOffset);
	SAILOR_PROFILE_END_BLOCK();
}

void GfxDeviceVulkan::SetMaterialParameter(RHI::CommandListPtr cmd, RHI::MaterialPtr material, const std::string& binding, const std::string& variable, const void* value, size_t size)
{
	SAILOR_PROFILE_FUNCTION();

	if (material->GetBindings()->HasBinding(binding))
	{
		auto device = m_vkInstance->GetMainDevice();
		auto& shaderBinding = material->GetBindings()->GetOrCreateShaderBinding(binding);
		UpdateShaderBingingVariable(cmd, shaderBinding, variable, value, size);
	}
}

void GfxDeviceVulkan::UpdateShaderBingingVariable(RHI::CommandListPtr cmd, RHI::ShaderBindingPtr shaderBinding, const std::string& variable, const void* value, size_t size)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	// All uniform buffers should be bound
	assert(shaderBinding->IsBind());

	RHI::ShaderLayoutBindingMember bindingLayout;
	assert(shaderBinding->FindVariableInUniformBuffer(variable, bindingLayout));

	UpdateShaderBinding(cmd, shaderBinding, value, size, bindingLayout.m_absoluteOffset);
}