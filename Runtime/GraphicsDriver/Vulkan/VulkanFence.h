#pragma once
#include "VulkanSemaphore.h"
#include "VulkanCommandBuffer.h"
#include "Memory/RefPtr.hpp"

namespace Sailor::GraphicsDriver::Vulkan
{
	class VulkanDevice;

	class VulkanFence final : public RHI::Resource
	{

	public:

		SAILOR_API VulkanFence(VulkanDevicePtr device, VkFenceCreateFlags flags = 0);

		SAILOR_API VkResult Wait(uint64_t timeout = UINT64_MAX) const;
		SAILOR_API VkResult Reset() const;
		SAILOR_API VkResult Status() const;

		SAILOR_API operator VkFence() const { return m_fence; }

		SAILOR_API const VkFence* GetHandle() const { return &m_fence; }

	protected:

		virtual ~VulkanFence();

		VulkanDevicePtr m_device;
		VkFence m_fence;
	};
}