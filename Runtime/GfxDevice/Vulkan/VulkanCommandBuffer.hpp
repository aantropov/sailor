#pragma once
#include "RHI/RHIResource.h"
#include "VulkanApi.h"

using namespace Sailor::RHI;

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanCommandBuffer : public Sailor::RHI::RHIResource
	{

	public:

		const VkCommandBuffer* GetHandle() const { return &m_commandBuffer; }
		operator VkCommandBuffer() const { return m_commandBuffer; }

		VulkanCommandBuffer(VkDevice device, VkCommandPool commandPool, VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY) :
			m_device(device),
			m_level(level),
			m_commandPool(commandPool)
		{
			VkCommandBufferAllocateInfo allocateInfo = {};
			allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
			allocateInfo.commandPool = commandPool;
			allocateInfo.level = level;
			allocateInfo.commandBufferCount = 1;

			VK_CHECK(vkAllocateCommandBuffers(device, &allocateInfo, &m_commandBuffer));
		}

		~VulkanCommandBuffer()
		{
			if (m_commandBuffer)
			{
				vkFreeCommandBuffers(m_device, m_commandPool, 1, &m_commandBuffer);
			}
		}

	protected:

		VkDevice m_device;
		VkCommandPool m_commandPool;
		VkCommandBuffer m_commandBuffer;
		VkCommandBufferLevel m_level;
	};
}