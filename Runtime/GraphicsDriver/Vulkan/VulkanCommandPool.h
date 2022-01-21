#pragma once
#include "VulkanApi.h"
#include "Memory/RefPtr.hpp"
#include "RHI/Types.h"

namespace Sailor::GraphicsDriver::Vulkan
{
	class VulkanDevice;
	
	class VulkanCommandPool final : public RHI::Resource
	{

	public:

		uint32_t GetQueueFamilyIndex() const { return m_queueFamilyIndex; }

		const VkCommandPool* GetHandle() const { return &m_commandPool; }
		operator VkCommandPool() const { return m_commandPool; }

		void Reset(VkCommandPoolResetFlags flags = VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT) const;

		VulkanCommandPool(VulkanDevicePtr device, uint32_t queueFamilyIndex, VkCommandPoolCreateFlags flags = 0);

		virtual ~VulkanCommandPool() override;

	protected:

		VulkanDevicePtr m_device;
		VkCommandPool m_commandPool;
		uint32_t m_queueFamilyIndex;
	};
}
