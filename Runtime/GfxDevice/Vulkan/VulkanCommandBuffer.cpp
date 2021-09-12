#include "VulkanCommandBuffer.h"
#include "VulkanCommandPool.h"
#include "Core/RefPtr.hpp"

using namespace Sailor;
using namespace Sailor::GfxDevice::Vulkan;

VulkanCommandBuffer::VulkanCommandBuffer(VkDevice device, Sailor::TRefPtr<VulkanCommandPool> commandPool, VkCommandBufferLevel level) :
	m_device(device),
	m_level(level),
	m_commandPool(commandPool)
{
	VkCommandBufferAllocateInfo allocateInfo = {};
	allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocateInfo.commandPool = *commandPool;
	allocateInfo.level = level;
	allocateInfo.commandBufferCount = 1;

	VK_CHECK(vkAllocateCommandBuffers(device, &allocateInfo, &m_commandBuffer));
}

VulkanCommandBuffer::~VulkanCommandBuffer()
{
	if (m_commandBuffer)
	{
		vkFreeCommandBuffers(m_device, *m_commandPool, 1, &m_commandBuffer);
	}
}

TRefPtr<VulkanCommandPool> VulkanCommandBuffer::GetCommandPool() const
{
	return m_commandPool;
}
