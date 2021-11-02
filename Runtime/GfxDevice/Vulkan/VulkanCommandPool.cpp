#include "VulkanCommandPool.h"
#include "VulkanDevice.h"

using namespace Sailor;
using namespace Sailor::GfxDevice::Vulkan;

void VulkanCommandPool::Reset(VkCommandPoolResetFlags flags) const
{
	VK_CHECK(vkResetCommandPool(*m_device, m_commandPool, flags));
}

VulkanCommandPool::VulkanCommandPool(VulkanDevicePtr device, uint32_t queueFamilyIndex, VkCommandPoolCreateFlags flags) :
	m_device(device)
{
	VkCommandPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.queueFamilyIndex = queueFamilyIndex;
	poolInfo.flags = flags;
	poolInfo.pNext = nullptr;

	VK_CHECK(vkCreateCommandPool(*m_device, &poolInfo, nullptr, &m_commandPool));
}

VulkanCommandPool::~VulkanCommandPool()
{
	if (m_commandPool)
	{
		vkDestroyCommandPool(*m_device, m_commandPool, nullptr);
	}
}
