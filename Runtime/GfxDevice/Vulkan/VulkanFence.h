#pragma once
#include "VulkanSemaphore.h"
#include "VulkanCommandBuffer.h"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanFence final : public TRefBase
	{

	public:

		VulkanFence(VkDevice device, VkFenceCreateFlags flags = 0);

		VkResult Wait(uint64_t timeout = UINT64_MAX) const;
		VkResult Reset() const;
		VkResult Status() const { return vkGetFenceStatus(m_device, m_fence); }

		operator VkFence() const { return m_fence; }

		const VkFence* GetHandle() const { return &m_fence; }

	protected:

		virtual ~VulkanFence();

		VkDevice m_device;
		VkFence m_fence;
	};
}