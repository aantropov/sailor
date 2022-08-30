#pragma once
#include "VulkanApi.h"
#include "Memory/RefPtr.hpp"
#include "RHI/Types.h"
#include "VulkanGraphicsDriver.h"
#include "Containers/Pair.h"

#ifdef MemoryBarrier
#undef MemoryBarrier
#endif

namespace Sailor::GraphicsDriver::Vulkan
{
	// TODO: Implement the possibility to reuse command lists (read: NOT one_time_submit for secondary command buffers?)
	class VulkanCommandBuffer final : public RHI::RHIResource
	{

	public:

		SAILOR_API const VkCommandBuffer* GetHandle() const { return &m_commandBuffer; }
		SAILOR_API operator VkCommandBuffer() const { return m_commandBuffer; }

		SAILOR_API VulkanCommandPoolPtr GetCommandPool() const;

		SAILOR_API VulkanCommandBuffer(VulkanDevicePtr device, VulkanCommandPoolPtr commandPool, VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY);

		SAILOR_API virtual ~VulkanCommandBuffer() override;

		SAILOR_API void BeginCommandList(VkCommandBufferUsageFlags flags = 0);
		SAILOR_API void BeginSecondaryCommandList(VulkanRenderPassPtr renderPass, uint32_t subpassIndex = 0, VkCommandBufferUsageFlags flags = 0);
		SAILOR_API void BeginSecondaryCommandList(const TVector<VkFormat>& colorAttachments, VkFormat depthStencilAttachment, VkCommandBufferUsageFlags flags = 0, VkRenderingFlags inheritanceFlags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT, bool bSupportMultisampling = true);

		SAILOR_API void EndCommandList();

		SAILOR_API void BeginRenderPassEx(const TVector<VulkanImageViewPtr>& colorAttachments,
			VulkanImageViewPtr depthStencilAttachment,
			VkRect2D renderArea,
			VkRenderingFlags renderingFlags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT_KHR,
			VkOffset2D offset = { 0,0 },
			bool bSupportMultisampling = true,
			bool bClearRenderTargets = true,
			VkClearValue clearColor = VulkanApi::DefaultClearColor);
		SAILOR_API void EndRenderPassEx();

		SAILOR_API void BeginRenderPass(VulkanRenderPassPtr renderPass,
			VulkanFramebufferPtr frameBuffer,
			VkExtent2D extent,
			VkSubpassContents content = VK_SUBPASS_CONTENTS_INLINE,
			VkOffset2D offset = { 0,0 },
			VkClearValue clearColor = VulkanApi::DefaultClearColor);

		SAILOR_API void EndRenderPass();

		SAILOR_API void SetDepthBias(float depthBiasConstantFactor, float depthBiasClamp = 0.0f, float depthBiasSlopeFactor = 0.0f);

		SAILOR_API void BindVertexBuffers(const TVector<VulkanBufferMemoryPtr>& buffers, TVector<VkDeviceSize> offsets = { 0 }, uint32_t firstBinding = 0, uint32_t bindingCount = 1);
		SAILOR_API void BindIndexBuffer(VulkanBufferMemoryPtr indexBuffer);

		SAILOR_API void BindVertexBuffers(const TVector<VulkanBufferPtr>& buffers, const TVector<VkDeviceSize>& offsets = { 0 }, uint32_t firstBinding = 0, uint32_t bindingCount = 1);
		SAILOR_API void BindIndexBuffer(VulkanBufferPtr indexBuffer, uint32_t offset = 0);
		SAILOR_API void BindDescriptorSet(VulkanPipelineLayoutPtr pipelineLayout, const TVector<VulkanDescriptorSetPtr>& descriptorSet);
		SAILOR_API void BindPipeline(VulkanPipelinePtr pipeline);

		SAILOR_API void DrawIndexedIndirect(VulkanBufferMemoryPtr buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride);
		SAILOR_API void DrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, uint32_t vertexOffset = 0, uint32_t firstInstance = 0);

		SAILOR_API void PushConstants(VulkanPipelineLayoutPtr pipelineLayout, size_t offset, size_t size, const void* ptr);
		SAILOR_API void Execute(VulkanCommandBufferPtr secondaryCommandBuffer);
		SAILOR_API void CopyBuffer(VulkanBufferMemoryPtr  src, VulkanBufferMemoryPtr dst, VkDeviceSize size, VkDeviceSize srcOffset = 0, VkDeviceSize dstOffset = 0);
		SAILOR_API void CopyBufferToImage(VulkanBufferPtr src, VulkanImagePtr image, uint32_t width, uint32_t height, VkDeviceSize srcOffset = 0);

		SAILOR_API void SetViewport(VulkanStateViewportPtr viewport);
		SAILOR_API void SetScissor(VulkanStateViewportPtr viewport);

		SAILOR_API void MemoryBarrier(VkAccessFlags srcAccess, VkAccessFlags dstAccess);
		SAILOR_API void ImageMemoryBarrier(VulkanImagePtr image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);

		SAILOR_API void Blit(VulkanImagePtr srcImage, VkImageLayout srcImageLayout, VulkanImagePtr dstImage, VkImageLayout dstImageLayout,
			uint32_t regionCount, const VkImageBlit* pRegions, VkFilter filter);

		SAILOR_API void GenerateMipMaps(VulkanImagePtr image);
		SAILOR_API void ClearDependencies();
		SAILOR_API void Reset();

		SAILOR_API void AddDependency(VulkanSemaphorePtr semaphore);
		SAILOR_API void AddDependency(VulkanCommandBufferPtr commandBuffer);
		SAILOR_API void AddDependency(TMemoryPtr<VulkanBufferMemoryPtr> ptr, TWeakPtr<VulkanBufferAllocator> allocator);

		SAILOR_API void BlitImage(VulkanImagePtr src, VulkanImagePtr dst, VkRect2D srcRegion, VkRect2D dstRegion, VkFilter filtration = VkFilter::VK_FILTER_LINEAR);

		SAILOR_API VkCommandBufferLevel GetLevel() const { return m_level; }

		// Used to get that the command list should be re-recorded
		SAILOR_API bool FitsViewport(const VkViewport& viewport) const;
		SAILOR_API bool IsRecorded() const { return m_bIsRecorded; }

	protected:

		SAILOR_API VkAccessFlags GetAccessFlags(VkImageLayout layout) const;
		SAILOR_API VkPipelineStageFlags GetPipelineStage(VkImageLayout layout) const;

		TVector<VulkanImagePtr> m_colorAttachmentDependencies;
		VulkanImagePtr m_depthStencilAttachmentDependency;

		TVector<VulkanBufferPtr> m_bufferDependencies;
		TVector<VulkanImagePtr> m_imageDependencies;
		TVector<VulkanDescriptorSetPtr> m_descriptorSetDependencies;
		TVector<VulkanPipelinePtr> m_pipelineDependencies;
		TVector<VulkanCommandBufferPtr> m_commandBufferDependencies;
		TVector<VulkanSemaphorePtr> m_semaphoreDependencies;
		TVector<TPair<TMemoryPtr<VulkanBufferMemoryPtr>, TWeakPtr<VulkanBufferAllocator>>> m_memoryPtrs;

		VulkanDevicePtr m_device;
		VulkanCommandPoolPtr m_commandPool;
		VulkanRenderPassPtr m_renderPass;
		VkCommandBuffer m_commandBuffer;
		VkCommandBufferLevel m_level;

		DWORD m_currentThreadId = 0;

		bool m_bIsRecorded = false;

		// We're tracking only viewport due to scissor almost never changed
		bool m_bHasViewport = false;
		VkViewport m_cachedViewportSettings{};
	};
}
