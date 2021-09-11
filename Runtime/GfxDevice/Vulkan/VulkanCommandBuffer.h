#pragma once
#include "VulkanApi.h"
namespace Sailor::GfxDevice::Vulkan
{
	class VulkanCommandPool;
	class VulkanCommandBuffer final : public TRefBase
	{

	public:

		const VkCommandBuffer* GetHandle() const { return &m_commandBuffer; }
		operator VkCommandBuffer() const { return m_commandBuffer; }

		TRefPtr <VulkanCommandPool> GetCommandPool() const { return m_commandPool; }

		VulkanCommandBuffer(VkDevice device, class TRefPtr<VulkanCommandPool> commandPool, VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY);

		virtual ~VulkanCommandBuffer() override;

	protected:

		VkDevice m_device;
		TRefPtr <VulkanCommandPool> m_commandPool;
		VkCommandBuffer m_commandBuffer;
		VkCommandBufferLevel m_level;
	};
}