#include "GfxDeviceVulkan.h"
#include "RHI/Texture.h"
#include "RHI/Fence.h"
#include "RHI/Mesh.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/Types.h"
#include "Platform/Win32/Window.h"
#include "VulkanApi.h"
#include "VulkanCommandBuffer.h"

using namespace Sailor;
using namespace Sailor::GfxDevice::Vulkan;

void GfxDeviceVulkan::Initialize(const Win32::Window* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug)
{
	GfxDevice::Vulkan::VulkanApi::Initialize(pViewport, msaaSamples, bIsDebug);

	m_vkInstance = GfxDevice::Vulkan::VulkanApi::GetInstance();
	m_vkInstance->GetMainDevice()->CreateVertexBuffer();
	m_vkInstance->GetMainDevice()->CreateGraphicsPipeline();

	GfxDevice::Vulkan::VulkanApi::Initialize(pViewport, msaaSamples, bIsDebug);
}

GfxDeviceVulkan::~GfxDeviceVulkan()
{
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
	const std::vector<TRefPtr<RHI::CommandList>>* primaryCommandBuffers,
	const std::vector<TRefPtr<RHI::CommandList>>* secondaryCommandBuffers) const
{
	std::vector<TRefPtr<VulkanCommandBuffer>> primaryBuffers;
	std::vector<TRefPtr<VulkanCommandBuffer>> secondaryBuffers;

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

void GfxDeviceVulkan::SubmitCommandList(TRefPtr<RHI::CommandList> commandList, TRefPtr<RHI::Fence> fence)
{
	m_vkInstance->GetMainDevice()->SubmitCommandBuffer(commandList->m_vulkan.m_commandBuffer, fence->m_vulkan.m_fence);
}

TRefPtr<RHI::Buffer> GfxDeviceVulkan::CreateBuffer(size_t size, RHI::EBufferUsageFlags usage)
{
	TRefPtr<RHI::Buffer> res = TRefPtr<RHI::Buffer>::Make();
	res->m_vulkan.m_buffer = m_vkInstance->CreateBuffer(m_vkInstance->GetMainDevice(), size, (uint16_t)usage, VkMemoryPropertyFlagBits::VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD);

	return res;
}

TRefPtr<RHI::CommandList> GfxDeviceVulkan::CreateBuffer(TRefPtr<RHI::Buffer>& outBuffer, const void* pData, size_t size, RHI::EBufferUsageFlags usage)
{
	outBuffer = TRefPtr<RHI::Buffer>::Make();
	TRefPtr<RHI::CommandList> cmdList = TRefPtr<RHI::CommandList>::Make();

	cmdList->m_vulkan.m_commandBuffer = m_vkInstance->CreateBuffer(outBuffer->m_vulkan.m_buffer,
		m_vkInstance->GetMainDevice(),
		pData, size, (uint16_t)usage);

	return cmdList;
}

TRefPtr<RHI::Buffer> GfxDeviceVulkan::CreateBuffer_Immediate(const void* pData, size_t size, RHI::EBufferUsageFlags usage)
{
	TRefPtr<RHI::Buffer> res = TRefPtr<RHI::Buffer>::Make();
	res->m_vulkan.m_buffer = m_vkInstance->CreateBuffer_Immediate(m_vkInstance->GetMainDevice(), pData, size, (uint32_t)usage);
	return res;
}

void GfxDeviceVulkan::CopyBuffer_Immediate(TRefPtr<RHI::Buffer> src, TRefPtr<RHI::Buffer> dst, size_t size)
{
	m_vkInstance->CopyBuffer_Immediate(m_vkInstance->GetMainDevice(), src->m_vulkan.m_buffer, dst->m_vulkan.m_buffer, size);
}

TRefPtr<RHI::Texture> GfxDeviceVulkan::CreateImage_Immediate(
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

	TRefPtr<RHI::Texture> res = TRefPtr<RHI::Texture>::Make();

	res->m_vulkan.m_image = m_vkInstance->CreateImage_Immediate(m_vkInstance->GetMainDevice(), pData, size, vkExtent, mipLevels, (VkImageType)type, (VkFormat)format, VK_IMAGE_TILING_OPTIMAL, (uint32_t)usage);
	return res;
}
