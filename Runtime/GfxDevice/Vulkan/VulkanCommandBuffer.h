#pragma once
#include "VulkanApi.h"
#include "Core/RefPtr.hpp"
#include "RHI/RHIResource.h"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanStateViewport;
	class VulkanCommandPool;
	class VulkanBuffer;
	class VulkanDevice;
	class VulkanRenderPass;
	class VulkanFramebuffer;
	class VulkanPipelineLayout;
	class VulkanPipeline;
	class VulkanDescriptorSet;

	class VulkanCommandBuffer final : public RHI::RHIResource
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
			VkClearValue clearColor = VulkanApi::DefaultClearColor);

		void EndRenderPass();

		void BindVertexBuffers(std::vector<TRefPtr<VulkanBuffer>> buffers, std::vector <VkDeviceSize> offsets = { 0 }, uint32_t firstBinding = 0, uint32_t bindingCount = 1);
		void BindIndexBuffer(TRefPtr<VulkanBuffer> indexBuffer);
		void BindDescriptorSet(TRefPtr<VulkanPipelineLayout> pipelineLayout, TRefPtr<VulkanDescriptorSet> descriptorSet);
		void BindPipeline(TRefPtr<VulkanPipeline> pipeline);

		void DrawIndexed(TRefPtr<VulkanBuffer> indexBuffer);

		void Execute(TRefPtr<VulkanCommandBuffer> secondaryCommandBuffer);
		void CopyBuffer(TRefPtr<VulkanBuffer>  src, TRefPtr<VulkanBuffer> dst, VkDeviceSize size, VkDeviceSize srcOffset = 0, VkDeviceSize dstOffset = 0);
		void CopyBufferToImage(TRefPtr<VulkanBuffer> src, TRefPtr<VulkanImage> image, uint32_t width, uint32_t height);

		void SetViewport(TRefPtr<const VulkanStateViewport> viewport);
		void SetScissor(TRefPtr<const VulkanStateViewport> viewport);

		void ImageMemoryBarrier(TRefPtr<VulkanImage> image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);

		void ClearDependencies();
		void Reset();

	protected:

		std::vector<TRefPtr<VulkanBuffer>> m_bufferDependencies;
		std::vector<TRefPtr<VulkanImage>> m_imageDependencies;

		TRefPtr<VulkanDevice> m_device;
		TRefPtr<VulkanCommandPool> m_commandPool;
		VkCommandBuffer m_commandBuffer;
		VkCommandBufferLevel m_level;
	};
}
