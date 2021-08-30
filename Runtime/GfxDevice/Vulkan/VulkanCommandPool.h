#pragma once
#include "RHI/RHIResource.h"
#include "VulkanApi.h"
#include "Core/RefPtr.hpp"

using namespace Sailor::RHI;

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanCommandPool : public Sailor::RHI::RHIResource
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