#pragma once
#include "VulkanApi.h"
#include "Core/RefPtr.hpp"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanCommandPool;
	class VulkanBuffer;
	class VulkanDevice;
	class VulkanRenderPass;
	class VulkanFramebuffer;

	class VulkanCommandBuffer final : public TRefBase
	{

	public:

		const VkCommandBuffer* GetHandle() const { return &m_commandBuffer; }
		operator VkCommandBuffer() const { return m_commandBuffer; }

		TRefPtr<VulkanCommandPool> GetCommandPool() const;

		VulkanCommandBuffer(TRefPtr<VulkanDevice> device, class TRefPtr<VulkanCommandPool> commandPool, VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY);

		virtual ~VulkanCommandBuffer() override;

		void BeginCommandList(VkCommandBufferUsageFlags flags = 0);
		void EndCommandList();

		void BeginRenderPass(TRefPtr<VulkanRenderPass> renderPass, 
			TRefPtr<VulkanFramebuffer> frameBuffer, 
			VkExtent2D extent, 
			VkOffset2D offset = { 0,0 }, 
			VkClearValue clearColor = { {{0.0f, 0.0f, 0.0f, 1.0f}} });
		
		void EndRenderPass();

		void BindVertexBuffers(std::vector<TRefPtr<VulkanBuffer>> buffers, std::vector <VkDeviceSize> offsets = {0}, uint32_t firstBinding = 0, uint32_t bindingCount = 1);
		void BindIndexBuffer(TRefPtr<VulkanBuffer> indexBuffer);
		void DrawIndexed(TRefPtr<VulkanBuffer> indexBuffer);

		void CopyBuffer(TRefPtr<VulkanBuffer>  src, TRefPtr<VulkanBuffer> dst, VkDeviceSize size, VkDeviceSize srcOffset = 0, VkDeviceSize dstOffset = 0);

	protected:

		TRefPtr<VulkanDevice> m_device;
		TRefPtr<VulkanCommandPool> m_commandPool;
		VkCommandBuffer m_commandBuffer;
		VkCommandBufferLevel m_level;
	};
}