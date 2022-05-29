#pragma once
#include "VulkanApi.h"
#include "Memory/RefPtr.hpp"
#include "RHI/Types.h"

namespace Sailor::GraphicsDriver::Vulkan
{
	class VulkanDevice;
	
	class VulkanCommandPool final : public RHI::RHIResource
	{

	public:

		SAILOR_API uint32_t GetQueueFamilyIndex() const { return m_queueFamilyIndex; }

		SAILOR_API const VkCommandPool* GetHandle() const { return &m_commandPool; }
		SAILOR_API operator VkCommandPool() const { return m_commandPool; }

		SAILOR_API void Reset(VkCommandPoolResetFlags flags = VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT) const;

		SAILOR_API VulkanCommandPool(VulkanDevicePtr device, uint32_t queueFamilyIndex, VkCommandPoolCreateFlags flags = 0);

		SAILOR_API virtual ~VulkanCommandPool() override;

	protected:

		VulkanDevicePtr m_device;
		VkCommandPool m_commandPool;
		uint32_t m_queueFamilyIndex;
	};
}
