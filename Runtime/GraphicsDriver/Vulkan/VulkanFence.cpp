#include "VulkanFence.h"
#include "VulkanDevice.h"
#include "Memory/RefPtr.hpp"

using namespace Sailor;
using namespace Sailor::GraphicsDriver::Vulkan;

VulkanFence::VulkanFence(VulkanDevicePtr device, VkFenceCreateFlags flags) :
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
	return CheckResult(m_device->m_waitForFences(*m_device, 1, &m_fence, VK_TRUE, timeout));
}

VkResult VulkanFence::Reset() const
{
	return CheckResult(vkResetFences(*m_device, 1, &m_fence));
}

VkResult VulkanFence::Status() const
{
	return CheckResult(m_device->m_getFenceStatus(*m_device, m_fence));
}

VkResult VulkanFence::CheckResult(VkResult result) const
{
	auto* device = m_device.GetRawPtr();
	if (result == VK_ERROR_DEVICE_LOST) device->m_bIsDeviceLost = true;
	// Completed work on a lost device cannot publish valid resource contents.
	return result == VK_SUCCESS && device->m_bIsDeviceLost ? VK_ERROR_DEVICE_LOST : result;
}
