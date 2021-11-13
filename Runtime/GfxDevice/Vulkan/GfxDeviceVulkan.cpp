#include "GfxDeviceVulkan.h"
#include "RHI/Texture.h"
#include "RHI/Fence.h"
#include "RHI/Mesh.h"
#include "RHI/Buffer.h"
#include "RHI/Shader.h"
#include "RHI/CommandList.h"
#include "RHI/Types.h"
#include "Platform/Win32/Window.h"
#include "VulkanApi.h"
#include "VulkanCommandBuffer.h"
#include "VulkanShaderModule.h"

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
	res->m_vulkan.m_buffer = m_vkInstance->CreateBuffer(m_vkInstance->GetMainDevice(), size, (uint16_t)usage, VkMemoryPropertyFlagBits::VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD);

	return res;
}

RHI::CommandListPtr GfxDeviceVulkan::CreateBuffer(RHI::BufferPtr& outBuffer, const void* pData, size_t size, RHI::EBufferUsageFlags usage)
{
	outBuffer = RHI::BufferPtr::Make();
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

RHI::MaterialPtr GfxDeviceVulkan::CreateMaterial(const RHI::RenderState& renderState)
{
	RHI::MaterialPtr res = RHI::MaterialPtr::Make(renderState);

	return res;
}
