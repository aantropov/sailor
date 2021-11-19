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
#include "Platform/Win32/Window.h"
#include "VulkanApi.h"
#include "VulkanImageView.h"
#include "VulkanImage.h"
#include "VulkanCommandBuffer.h"
#include "VulkanPipeline.h"
#include "VulkanShaderModule.h"
#include "VulkanBufferMemory.h"

using namespace Sailor;
using namespace Sailor::GfxDevice::Vulkan;

void GfxDeviceVulkan::Initialize(const Win32::Window* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug)
{
	GfxDevice::Vulkan::VulkanApi::Initialize(pViewport, msaaSamples, bIsDebug);
	m_vkInstance = GfxDevice::Vulkan::VulkanApi::GetInstance();
}

GfxDeviceVulkan::~GfxDeviceVulkan()
{
	m_trackedFences.clear();
	m_uniformBuffers.clear();
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

	//if we have fence and that is null we should create device resource
	if (!fence->m_vulkan.m_fence)
	{
		fence->m_vulkan.m_fence = VulkanFencePtr::Make(m_vkInstance->GetMainDevice());
	}

	std::vector<VulkanSemaphorePtr> signal;
	std::vector<VulkanSemaphorePtr> wait;

	if (signalSemaphore)
	{
		commandList->m_vulkan.m_commandBuffer->AddSemaphoreDependency(signalSemaphore->m_vulkan.m_semaphore);
		signal.push_back(signalSemaphore->m_vulkan.m_semaphore);
	}

	if (waitSemaphore)
	{
		commandList->m_vulkan.m_commandBuffer->AddSemaphoreDependency(waitSemaphore->m_vulkan.m_semaphore);
		wait.push_back(waitSemaphore->m_vulkan.m_semaphore);
	}

	m_vkInstance->GetMainDevice()->SubmitCommandBuffer(commandList->m_vulkan.m_commandBuffer, fence->m_vulkan.m_fence, signal, wait);

	// Fence should hold command list during execution
	fence->AddDependency(commandList);

	// We should remove fence after execution
	TrackPendingCommandList_ThreadSafe(fence);
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
					valueBinding.m_buffer, valueBinding.m_offset, valueBinding.m_size);

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

RHI::MaterialPtr GfxDeviceVulkan::CreateMaterial(const RHI::RenderState& renderState, const UID& shader, const std::vector<std::string>& defines)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	std::vector<VulkanDescriptorSetLayoutPtr> descriptorSetLayouts;
	std::vector<RHI::ShaderLayoutBinding> bindings;

	// We need debug shaders to get full names from reflection
	RHI::ShaderByteCode debugVertexSpirv;
	RHI::ShaderByteCode debugFragmentSpirv;
	ShaderCompiler::GetSpirvCode(shader, defines, debugVertexSpirv, debugFragmentSpirv, true);
	auto debugVertexShader = CreateShader(RHI::EShaderStage::Vertex, debugVertexSpirv);
	auto debugFragmentShader = CreateShader(RHI::EShaderStage::Fragment, debugFragmentSpirv);
	VulkanApi::CreateDescriptorSetLayouts(device, { debugVertexShader->m_vulkan.m_shader, debugFragmentShader->m_vulkan.m_shader },
		descriptorSetLayouts, bindings);

#ifdef _DEBUG
	const bool bIsDebug = true;
#else
	const bool bIsDebug = false;
#endif

	RHI::ShaderByteCode vertexSpirv;
	RHI::ShaderByteCode fragmentSpirv;
	ShaderCompiler::GetSpirvCode(shader, defines, vertexSpirv, fragmentSpirv, bIsDebug);
	auto vertexShader = CreateShader(RHI::EShaderStage::Vertex, vertexSpirv);
	auto fragmentShader = CreateShader(RHI::EShaderStage::Fragment, fragmentSpirv);

	RHI::MaterialPtr res = RHI::MaterialPtr::Make(renderState, vertexShader, fragmentShader);

	// TODO: Rearrange descriptorSetLayouts to support vector of descriptor sets
	auto pipelineLayout = VulkanPipelineLayoutPtr::Make(device,
		descriptorSetLayouts,
		std::vector<VkPushConstantRange>(),
		0);

	res->m_vulkan.m_pipeline = VulkanPipelinePtr::Make(device,
		pipelineLayout,
		std::vector{ vertexShader->m_vulkan.m_shader, fragmentShader->m_vulkan.m_shader },
		device->GetPipelineBuilder()->BuildPipeline(renderState),
		0);

	res->m_vulkan.m_pipeline->m_renderPass = device->GetRenderPass();
	res->m_vulkan.m_pipeline->Compile();

	auto shaderBindings = CreateShaderBindings();
	res->SetBindings(shaderBindings);

	for (uint32_t i = 0; i < bindings.size(); i++)
	{
		auto& layoutBinding = bindings[i];
		if (layoutBinding.m_set == 0)
		{
			// We skip 0 layout, it is frameData and would be binded in a different way
			continue;
		}
		const auto& layoutBindings = descriptorSetLayouts[layoutBinding.m_set]->m_descriptorSetLayoutBindings;

		auto it = std::find_if(layoutBindings.begin(), layoutBindings.end(), [&layoutBinding](const auto& bind) { return bind.binding == layoutBinding.m_location; });
		if (it == layoutBindings.end())
		{
			continue;
		}

		auto& vkLayoutBinding = *it;
		auto& binding = shaderBindings->GetOrCreateShaderBinding(layoutBinding.m_name);

		if (layoutBinding.m_type == RHI::EShaderBindingType::UniformBuffer)
		{
			auto& uniformAllocator = GetUniformBufferAllocator(layoutBinding.m_name);
			binding->m_vulkan.m_valueBinding = uniformAllocator.Allocate(layoutBinding.m_size, device->GetUboOffsetAlignment(layoutBinding.m_size));
			binding->m_vulkan.m_descriptorSetLayout = vkLayoutBinding;
			binding->SetMembersInfo(layoutBinding);
		}
	}

	shaderBindings->SetLayoutShaderBindings(bindings);
	UpdateDescriptorSet(shaderBindings);

	return res;
}

VulkanUniformBufferAllocator& GfxDeviceVulkan::GetUniformBufferAllocator(const std::string& uniformTypeId)
{
	SAILOR_PROFILE_FUNCTION();

	auto it = m_uniformBuffers.find(uniformTypeId);
	if (it != m_uniformBuffers.end())
	{
		return (*it).second;
	}

	auto& uniformAllocator = m_uniformBuffers[uniformTypeId];
	uniformAllocator.GetGlobalAllocator().SetUsage(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	uniformAllocator.GetGlobalAllocator().SetMemoryProperties(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

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

void GfxDeviceVulkan::AddUniformBufferToShaderBindings(RHI::ShaderBindingSetPtr& pShaderBindings, const std::string& name, size_t size, uint32_t shaderBinding)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();

	RHI::ShaderBindingPtr binding = pShaderBindings->GetOrCreateShaderBinding(name);
	binding->m_vulkan.m_valueBinding = GetUniformBufferAllocator(name).Allocate(size, device->GetUboOffsetAlignment(size));
	auto valueBinding = *(binding->m_vulkan.m_valueBinding);

	RHI::ShaderLayoutBinding layout;
	layout.m_location = shaderBinding;
	layout.m_name = name;
	layout.m_size = (uint32_t)size;
	layout.m_type = RHI::EShaderBindingType::UniformBuffer;

	binding->SetMembersInfo(layout);
	binding->m_vulkan.m_descriptorSetLayout = VulkanApi::CreateDescriptorSetLayoutBinding(layout.m_location, (VkDescriptorType)layout.m_type);

	UpdateDescriptorSet(pShaderBindings);
}

void GfxDeviceVulkan::AddSamplerToShaderBindings(RHI::ShaderBindingSetPtr& pShaderBindings, const std::string& name, RHI::TexturePtr texture, uint32_t shaderBinding)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();
	RHI::ShaderBindingPtr binding = pShaderBindings->GetOrCreateShaderBinding(name);

	RHI::ShaderLayoutBinding layout;
	layout.m_location = shaderBinding;
	layout.m_name = name;
	layout.m_type = RHI::EShaderBindingType::CombinedImageSampler;

	binding->SetMembersInfo(layout);
	binding->m_vulkan.m_descriptorSetLayout = VulkanApi::CreateDescriptorSetLayoutBinding(layout.m_location, (VkDescriptorType)layout.m_type);
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
				return descriptor->GetBinding() == it->m_location;
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
			textureBinding->m_vulkan.m_descriptorSetLayout = VulkanApi::CreateDescriptorSetLayoutBinding(it->m_location, (VkDescriptorType)it->m_type);
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

	auto& stagingMemoryAllocator = device->GetMemoryAllocator((VkMemoryPropertyFlags)(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT), requirements);
	auto data = stagingMemoryAllocator.Allocate(size, requirements.alignment);

	VulkanBufferPtr stagingBuffer = VulkanBufferPtr::Make(device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_CONCURRENT);
	stagingBuffer->Compile();
	VK_CHECK(stagingBuffer->Bind(data));

	stagingBuffer->GetMemoryDevice()->Copy((*data).m_offset, size, pData);

	cmd->m_vulkan.m_commandBuffer->CopyBuffer(stagingBuffer, dstBuffer, size, 0, binding.m_offset + variableOffset);
}

void GfxDeviceVulkan::SetMaterialParameter(RHI::CommandListPtr cmd, RHI::MaterialPtr material, const std::string& binding, const std::string& variable, const void* value, size_t size)
{
	SAILOR_PROFILE_FUNCTION();

	auto device = m_vkInstance->GetMainDevice();
	auto& shaderBinding = material->GetBindings()->GetOrCreateShaderBinding(binding);
	UpdateShaderBingingVariable(cmd, shaderBinding, variable, value, size);
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