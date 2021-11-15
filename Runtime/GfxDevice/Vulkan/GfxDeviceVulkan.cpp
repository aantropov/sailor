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
	m_vkInstance->GetMainDevice()->CreateGraphicsPipeline();
}

GfxDeviceVulkan::~GfxDeviceVulkan()
{
	m_trackedFences.clear();
	GfxDevice::Vulkan::VulkanApi::Shutdown();
}

bool GfxDeviceVulkan::ShouldFixLostDevice(const Win32::Window* pViewport)
{
	return m_vkInstance->GetMainDevice()->ShouldFixLostDevice(pViewport);
}

void GfxDeviceVulkan::FixLostDevice(const Win32::Window* pViewport)
{
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
	const std::vector<RHI::CommandListPtr>* secondaryCommandBuffers) const
{
	std::vector<VulkanCommandBufferPtr> primaryBuffers;
	std::vector<VulkanCommandBufferPtr> secondaryBuffers;

	if (primaryCommandBuffers != nullptr)
	{
		primaryBuffers.resize(primaryCommandBuffers->size());
		for (auto& buffer : *primaryCommandBuffers)
		{
			primaryBuffers.push_back(buffer->m_vulkan.m_commandBuffer);
		}
	}

	if (secondaryCommandBuffers != nullptr)
	{
		secondaryBuffers.resize(secondaryCommandBuffers->size());
		for (auto& buffer : *secondaryCommandBuffers)
		{
			secondaryBuffers.push_back(buffer->m_vulkan.m_commandBuffer);
		}
	}

	return m_vkInstance->PresentFrame(state, &primaryBuffers, &secondaryBuffers);
}

void GfxDeviceVulkan::WaitIdle()
{
	m_vkInstance->WaitIdle();
}

void GfxDeviceVulkan::SubmitCommandList(RHI::CommandListPtr commandList, RHI::FencePtr fence)
{
	//if we have fence and that is null we should create device resource
	if (!fence->m_vulkan.m_fence)
	{
		fence->m_vulkan.m_fence = VulkanFencePtr::Make(m_vkInstance->GetMainDevice());
	}

	m_vkInstance->GetMainDevice()->SubmitCommandBuffer(commandList->m_vulkan.m_commandBuffer, fence->m_vulkan.m_fence);

	// Fence should hold command list during execution
	fence->AddDependency(commandList);
}

RHI::BufferPtr GfxDeviceVulkan::CreateBuffer(size_t size, RHI::EBufferUsageFlags usage)
{
	RHI::BufferPtr res = RHI::BufferPtr::Make();
	res->m_vulkan.m_buffer = m_vkInstance->CreateBuffer(m_vkInstance->GetMainDevice(), size, (uint16_t)usage, VkMemoryPropertyFlagBits::VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	return res;
}

RHI::CommandListPtr GfxDeviceVulkan::CreateBuffer(RHI::BufferPtr& outBuffer, const void* pData, size_t size, RHI::EBufferUsageFlags usage)
{
	outBuffer = CreateBuffer(size, usage);
	RHI::CommandListPtr cmdList = RHI::CommandListPtr::Make();

	cmdList->m_vulkan.m_commandBuffer = m_vkInstance->CreateBuffer(outBuffer->m_vulkan.m_buffer,
		m_vkInstance->GetMainDevice(),
		pData, size, (uint16_t)usage);

	return cmdList;
}

RHI::ShaderPtr GfxDeviceVulkan::CreateShader(RHI::EShaderStage shaderStage, const RHI::ShaderByteCode& shaderSpirv)
{
	auto res = RHI::ShaderPtr::Make(shaderStage);
	res->m_vulkan.m_shader = VulkanShaderStagePtr::Make((VkShaderStageFlagBits)shaderStage, "main", m_vkInstance->GetMainDevice(), shaderSpirv);
	return res;
}

RHI::BufferPtr GfxDeviceVulkan::CreateBuffer_Immediate(const void* pData, size_t size, RHI::EBufferUsageFlags usage)
{
	RHI::BufferPtr res = RHI::BufferPtr::Make();
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
	RHI::ETextureUsageFlags usage)
{
	VkExtent3D vkExtent;
	vkExtent.width = extent.x;
	vkExtent.height = extent.y;
	vkExtent.depth = extent.z;

	RHI::TexturePtr res = TRefPtr<RHI::Texture>::Make();
	res->m_vulkan.m_image = m_vkInstance->CreateImage_Immediate(m_vkInstance->GetMainDevice(), pData, size, vkExtent, mipLevels, (VkImageType)type, (VkFormat)format, VK_IMAGE_TILING_OPTIMAL, (uint32_t)usage);
	return res;
}

RHI::TexturePtr GfxDeviceVulkan::CreateImage(
	const void* pData,
	size_t size,
	glm::ivec3 extent,
	uint32_t mipLevels,
	RHI::ETextureType type,
	RHI::ETextureFormat format,
	RHI::ETextureUsageFlags usage)
{
	RHI::TexturePtr outTexture = RHI::TexturePtr::Make();

	VkExtent3D vkExtent;
	vkExtent.width = extent.x;
	vkExtent.height = extent.y;
	vkExtent.depth = extent.z;

	RHI::CommandListPtr cmdList = RHI::CommandListPtr::Make();
	cmdList->m_vulkan.m_commandBuffer = m_vkInstance->CreateImage(outTexture->m_vulkan.m_image, m_vkInstance->GetMainDevice(), pData, size, vkExtent, mipLevels, (VkImageType)type, (VkFormat)format, VK_IMAGE_TILING_OPTIMAL, (uint32_t)usage);

	TRefPtr<RHI::Fence> fenceUpdateRes = TRefPtr<RHI::Fence>::Make();

	// Submit cmd lists
	SAILOR_ENQUEUE_JOB_RENDER_THREAD("Create texture",
		([this, fenceUpdateRes, cmdList]()
			{
				SubmitCommandList(cmdList, fenceUpdateRes);
			}));

	TrackDelayedInitialization(outTexture.GetRawPtr(), fenceUpdateRes);

	return outTexture;
}

void GfxDeviceVulkan::UpdateDescriptorSet(RHI::MaterialPtr material)
{
	auto device = m_vkInstance->GetMainDevice();
	std::vector<VulkanDescriptorPtr> descriptors;

	for (const auto& binding : material->GetShaderBindings())
	{
		if (binding.second->IsBind())
		{
			if (binding.second->m_vulkan.m_textureBinding)
			{
				auto imageView = binding.second->m_vulkan.m_textureBinding;
				auto descr = VulkanDescriptorImagePtr::Make(binding.second, 0,
					device->GetSamplers()->GetSampler(RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Clamp, true),
					imageView);

				descriptors.push_back(descr);
			}
			else if (binding.second->m_vulkan.m_valueBinding)
			{
				auto& valueBinding = (*binding.second->m_vulkan.m_valueBinding);
				auto descr = VulkanDescriptorBufferPtr::Make(binding.second, 0,
					valueBinding.m_buffer, valueBinding.m_offset, valueBinding.m_size);
				
				descriptors.push_back(descr);
			}
		}
	}

	material->m_vulkan.m_descriptorSet = VulkanDescriptorSetPtr::Make(device,
		device->GetThreadContext().m_descriptorPool,
		material->m_vulkan.m_pipeline->m_layout->m_descriptionSetLayouts[0],
		descriptors);

	material->m_vulkan.m_descriptorSet->Compile();

}

RHI::MaterialPtr GfxDeviceVulkan::CreateMaterial(const RHI::RenderState& renderState, RHI::ShaderPtr vertexShader, RHI::ShaderPtr fragmentShader)
{
	auto device = m_vkInstance->GetMainDevice();

	std::vector<VulkanDescriptorSetLayoutPtr> descriptorSetLayouts;
	std::vector<RHI::ShaderLayoutBinding> bindings;

	VulkanApi::CreateDescriptorSetLayouts(device, { vertexShader->m_vulkan.m_shader, fragmentShader->m_vulkan.m_shader }, descriptorSetLayouts, bindings);

	RHI::MaterialPtr res = RHI::MaterialPtr::Make(renderState, vertexShader, fragmentShader);

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

	for (auto& layoutBinding : bindings)
	{
		auto& binding = res->GetOrCreateShaderBinding(layoutBinding.m_name);

		// That is reserved by render pipeline
		if (layoutBinding.m_location != 0 && layoutBinding.m_name != "transform")
		{
			if (layoutBinding.m_type == RHI::EShaderBindingType::UniformBuffer)
			{
				auto& uniformAllocator = GetUniformBufferAllocator(layoutBinding.m_name);
				binding->m_vulkan.m_valueBinding = uniformAllocator.Allocate(layoutBinding.m_size, device->GetUboOffsetAlignment(layoutBinding.m_size));
			}
		}
	}

	res->SetLayoutShaderBindings(bindings);
	UpdateDescriptorSet(res);

	return res;
}

VulkanUniformBufferAllocator& GfxDeviceVulkan::GetUniformBufferAllocator(const std::string& uniformTypeId)
{
	auto it = m_uniformBuffers.find(uniformTypeId);
	if (it != m_uniformBuffers.end())
	{
		return (*it).second;
	}

	auto& uniformAllocator = m_uniformBuffers[uniformTypeId];
	uniformAllocator.GetGlobalAllocator().SetUsage(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
	uniformAllocator.GetGlobalAllocator().SetMemoryProperties(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

	return uniformAllocator;
}

void GfxDeviceVulkan::SetMaterialParameter(RHI::MaterialPtr material, const std::string& parameter, const void* value, size_t size)
{
	auto device = m_vkInstance->GetMainDevice();

	RHI::CommandListPtr commandList = RHI::CommandListPtr::Make();
	commandList->m_vulkan.m_commandBuffer = Vulkan::VulkanCommandBufferPtr::Make(device, device->GetThreadContext().m_commandPool, VkCommandBufferLevel::VK_COMMAND_BUFFER_LEVEL_SECONDARY);

	auto& shaderBinding = material->GetOrCreateShaderBinding(parameter);
	bool bShouldUpdateDescriptorSet = false;

	// All uniform buffers should be bound
	assert(shaderBinding->IsBind());

	SetMaterialParameter(commandList, shaderBinding, value, size);

	SubmitCommandList_Immediate(commandList);
}

void GfxDeviceVulkan::SetMaterialParameter(RHI::MaterialPtr material, const std::string& parameter, RHI::TexturePtr value)
{
	const auto& layoutBindings = material->GetLayoutBindings();

	auto it = std::find_if(layoutBindings.begin(), layoutBindings.end(), [&parameter](const RHI::ShaderLayoutBinding& shaderLayoutBinding)
		{
			return shaderLayoutBinding.m_name == parameter;
		});

	if (it != layoutBindings.end())
	{
		auto& descriptors = material->m_vulkan.m_descriptorSet->m_descriptors;
		auto descrIt = std::find_if(descriptors.begin(), descriptors.end(), [&it](const VulkanDescriptorPtr& descriptor)
			{
				return descriptor->GetBinding() == it->m_location;
			});

		if (descrIt != descriptors.end())
		{
			// Should we create a new one descriptorSet to avoid race condition?
			auto descriptor = (*descrIt).DynamicCast<VulkanDescriptorImage>();
			descriptor->SetImageView(value->m_vulkan.m_imageView);
			material->m_vulkan.m_descriptorSet->Compile();
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

void GfxDeviceVulkan::SetMaterialParameter(RHI::CommandListPtr cmd, RHI::ShaderBindingPtr parameter, const void* pData, size_t size)
{
	auto device = m_vkInstance->GetMainDevice();
	auto& binding = parameter->m_vulkan.m_valueBinding;
	auto dstBuffer = binding.m_ptr.m_buffer;

	const auto requirements = dstBuffer->GetMemoryRequirements();

	auto& stagingMemoryAllocator = device->GetMemoryAllocator((VkMemoryPropertyFlags)(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT), requirements);
	auto data = stagingMemoryAllocator.Allocate(size, requirements.alignment);

	VulkanBufferPtr stagingBuffer = VulkanBufferPtr::Make(device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_CONCURRENT);
	stagingBuffer->Compile();
	VK_CHECK(stagingBuffer->Bind(data));

	stagingBuffer->GetMemoryDevice()->Copy((*data).m_offset, size, pData);
	cmd->m_vulkan.m_commandBuffer->CopyBuffer(stagingBuffer, dstBuffer, size, 0, binding.m_offset);
}
