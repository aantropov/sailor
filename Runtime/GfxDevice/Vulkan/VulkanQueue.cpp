#include "VulkanQueue.h"
#include "VulkanFence.h"
#include "Core/RefPtr.hpp"

using namespace Sailor;
using namespace Sailor::GfxDevice::Vulkan;

VulkanQueue::VulkanQueue(VkQueue queue, uint32_t queueFamilyIndex, uint32_t queueIndex) :
	m_queue(queue),
	m_queueFamilyIndex(queueFamilyIndex),
	m_queueIndex(queueIndex)
{
}

VulkanQueue::~VulkanQueue()
{
}

VkResult VulkanQueue::Submit(const std::vector<VkSubmitInfo>& submitInfos, TRefPtr<VulkanFence> fence)
{
	std::scoped_lock<std::mutex> guard(m_mutex);
	return vkQueueSubmit(m_queue, static_cast<uint32_t>(submitInfos.size()), submitInfos.data(), fence ? (VkFence)*fence : VK_NULL_HANDLE);
}

VkResult VulkanQueue::Submit(const VkSubmitInfo& submitInfo, TRefPtr<VulkanFence> fence)
{
	std::scoped_lock<std::mutex> guard(m_mutex);
	return vkQueueSubmit(m_queue, 1, &submitInfo, fence ? (VkFence)*fence : VK_NULL_HANDLE);
}

VkResult VulkanQueue::Present(const VkPresentInfoKHR& info)
{
	std::scoped_lock<std::mutex> guard(m_mutex);
	return vkQueuePresentKHR(m_queue, &info);
}

VkResult VulkanQueue::WaitIdle()
{
	std::scoped_lock<std::mutex> guard(m_mutex);
	return vkQueueWaitIdle(m_queue);
}