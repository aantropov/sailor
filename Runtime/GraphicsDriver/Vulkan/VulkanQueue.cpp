#include "VulkanQueue.h"
#include "VulkanFence.h"
#include "Memory/RefPtr.hpp"

using namespace Sailor;
using namespace Sailor::GraphicsDriver::Vulkan;

VulkanQueue::VulkanQueue(VkQueue queue, uint32_t queueFamilyIndex, uint32_t queueIndex) :
	m_queue(queue),
	m_queueFamilyIndex(queueFamilyIndex),
	m_queueIndex(queueIndex)
{
}

VulkanQueue::~VulkanQueue()
{
}

VkResult VulkanQueue::Submit(const TVector<VkSubmitInfo>& submitInfos, VulkanFencePtr fence) const
{
	m_lock.Lock();
	auto res = vkQueueSubmit(m_queue, static_cast<uint32_t>(submitInfos.Num()), submitInfos.GetData(), fence ? (VkFence)*fence : VK_NULL_HANDLE);
	m_lock.Unlock();

	return res;
}

VkResult VulkanQueue::Submit(const VkSubmitInfo& submitInfo, VulkanFencePtr fence) const
{
	m_lock.Lock();
	auto res = vkQueueSubmit(m_queue, 1, &submitInfo, fence ? (VkFence)*fence : VK_NULL_HANDLE);
	m_lock.Unlock();

	return res;
}

VkResult VulkanQueue::Present(const VkPresentInfoKHR& info)
{
	m_lock.Lock();
	auto res = vkQueuePresentKHR(m_queue, &info);
	m_lock.Unlock();

	return res;
}

VkResult VulkanQueue::WaitIdle()
{
	m_lock.Lock();
	auto res = vkQueueWaitIdle(m_queue);
	m_lock.Unlock();

	return res;
}