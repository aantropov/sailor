#pragma once
#include "VulkanSemaphore.h"
#include "VulkanCommandBuffer.h"
#include "Memory/RefPtr.hpp"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanDevice;

	class VulkanFence final : public RHI::Resource
	{

	public:

		VulkanFence(VulkanDevicePtr device, VkFenceCreateFlags flags = 0);

		VkResult Wait(uint64_t timeout = UINT64_MAX) const;
		VkResult Reset() const;
		VkResult Status() const;

		operator VkFence() const { return m_fence; }

		const VkFence* GetHandle() const { return &m_fence; }

	protected:

		virtual ~VulkanFence();

		VulkanDevicePtr m_device;
		VkFence m_fence;
	};
}