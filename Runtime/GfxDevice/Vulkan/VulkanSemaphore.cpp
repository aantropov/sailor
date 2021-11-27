#include "Memory/RefPtr.hpp"
#include "VulkanDevice.h"
#include "VulkanSemaphore.h"

using namespace Sailor;
using namespace Sailor::GfxDevice::Vulkan;

VulkanSemaphore::VulkanSemaphore(VulkanDevicePtr device, VkPipelineStageFlags pipelineStageFlags, void* pNextCreateInfo) :
	m_pipelineStageFlags(pipelineStageFlags),
	m_device(device)
{
	VkSemaphoreCreateInfo semaphoreInfo = {};
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	semaphoreInfo.pNext = pNextCreateInfo;

	VK_CHECK(vkCreateSemaphore(*device, &semaphoreInfo, nullptr, &m_semaphore));
}

VulkanSemaphore::~VulkanSemaphore()
{
	if (m_semaphore)
	{
		vkDestroySemaphore(*m_device, m_semaphore, nullptr);
	}
}