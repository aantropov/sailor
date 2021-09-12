#pragma once
#include "VulkanApi.h"
#include "Core/RefPtr.hpp"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanCommandPool;
	class VulkanBuffer;
	class VulkanDevice;

	class VulkanCommandBuffer final : public TRefBase
	{

	public:

		const VkCommandBuffer* GetHandle() const { return &m_commandBuffer; }
		operator VkCommandBuffer() const { return m_commandBuffer; }

		TRefPtr<VulkanCommandPool> GetCommandPool() const;

		VulkanCommandBuffer(TRefPtr<VulkanDevice> device, class TRefPtr<VulkanCommandPool> commandPool, VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY);

		virtual ~VulkanCommandBuffer() override;

		void BeginCommandList();
		void EndCommandList();

		void CopyBuffer(TRefPtr<VulkanBuffer>  src, TRefPtr<VulkanBuffer> dst, VkDeviceSize size, VkDeviceSize srcOffset = 0, VkDeviceSize dstOffset = 0);

	protected:

		TRefPtr<VulkanDevice> m_device;
		TRefPtr <VulkanCommandPool> m_commandPool;
		VkCommandBuffer m_commandBuffer;
		VkCommandBufferLevel m_level;
	};
}