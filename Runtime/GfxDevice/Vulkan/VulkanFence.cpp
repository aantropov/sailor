#include "VulkanFence.h"

using namespace Sailor::GfxDevice::Vulkan;

VulkanFence::VulkanFence(VkDevice device, VkFenceCreateFlags flags) :
	m_device(device)
{
	VkFenceCreateInfo createFenceInfo = {};
	createFenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	createFenceInfo.flags = flags;
	createFenceInfo.pNext = nullptr;

	VK_CHECK(vkCreateFence(m_device, &createFenceInfo, nullptr, &m_fence));
}

VulkanFence::~VulkanFence()
{
	if (m_fence)
	{
		vkDestroyFence(m_device, m_fence, nullptr);
	}
}

VkResult VulkanFence::Wait(uint64_t timeout) const
{
	return vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, timeout);
}

VkResult VulkanFence::Reset() const
{
	return vkResetFences(m_device, 1, &m_fence);
}