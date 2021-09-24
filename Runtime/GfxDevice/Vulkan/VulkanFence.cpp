#include "VulkanFence.h"
#include "VulkanDevice.h"
#include "Core/RefPtr.hpp"

using namespace Sailor;
using namespace Sailor::GfxDevice::Vulkan;

VulkanFence::VulkanFence(TRefPtr<VulkanDevice> device, VkFenceCreateFlags flags) :
	m_device(device)
{
	VkFenceCreateInfo createFenceInfo = {};
	createFenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	createFenceInfo.flags = flags;
	createFenceInfo.pNext = nullptr;

	VK_CHECK(vkCreateFence(*m_device, &createFenceInfo, nullptr, &m_fence));
}

VulkanFence::~VulkanFence()
{
	if (m_fence)
	{
		vkDestroyFence(*m_device, m_fence, nullptr);
	}

	m_device.Clear();
}

VkResult VulkanFence::Wait(uint64_t timeout) const
{
	return vkWaitForFences(*m_device, 1, &m_fence, VK_TRUE, timeout);
}

VkResult VulkanFence::Reset() const
{
	return vkResetFences(*m_device, 1, &m_fence);
}

VkResult VulkanFence::Status() const
{
	return vkGetFenceStatus(*m_device, m_fence);
}
