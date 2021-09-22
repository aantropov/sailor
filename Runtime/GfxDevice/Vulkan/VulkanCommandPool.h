#pragma once
#include "VulkanApi.h"
#include "Core/RefPtr.hpp"
#include "RHI/RHIResource.h"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanCommandPool final : public RHI::RHIResource
	{

	public:

		const VkCommandPool* GetHandle() const { return &m_commandPool; }
		operator VkCommandPool() const { return m_commandPool; }

		void Reset(VkCommandPoolResetFlags flags = VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT) const;

		VulkanCommandPool(VkDevice device, uint32_t queueFamilyIndex, VkCommandPoolCreateFlags flags = 0);

		virtual ~VulkanCommandPool() override;

	protected:

		VkDevice m_device;
		VkCommandPool m_commandPool;
	};
}
