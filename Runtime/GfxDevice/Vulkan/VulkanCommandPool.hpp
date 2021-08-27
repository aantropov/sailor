#pragma once
#include "RHI/RHIResource.h"
#include "VulkanApi.h"

using namespace Sailor::RHI;

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanCommandPool : public Sailor::RHI::RHIResource
	{

	public:

		const VkCommandPool* GetHandle() const { return &m_commandPool; }
		operator VkCommandPool() const { return m_commandPool; }

		void Reset(VkCommandPoolResetFlags flags = VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT) const 
		{
			VK_CHECK(vkResetCommandPool(m_device, m_commandPool, flags)); 
		}

		VulkanCommandPool(VkDevice device, uint32_t queueFamilyIndex, VkCommandPoolCreateFlags flags = 0) :
			m_device(device)
		{
			VkCommandPoolCreateInfo poolInfo = {};
			poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
			poolInfo.queueFamilyIndex = queueFamilyIndex;
			poolInfo.flags = flags;
			poolInfo.pNext = nullptr;

			VK_CHECK(vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool));
		}

		virtual ~VulkanCommandPool() override
		{
			if (m_commandPool)
			{
				vkDestroyCommandPool(m_device, m_commandPool, nullptr);
			}
		}

	protected:

		VkDevice m_device;
		VkCommandPool m_commandPool;
	};
}