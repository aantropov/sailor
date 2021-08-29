#pragma once
#include "RHI/RHIResource.h"
#include "VulkanSemaphore.h"
#include "VulkanCommandBuffer.h"

using namespace Sailor::RHI;

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanFence : public Sailor::RHI::RHIResource
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