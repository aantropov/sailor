#include <array>

#include "VulkanApi.h"
#include "VulkanCommandBuffer.h"
#include "VulkanCommandPool.h"
#include "VulkanBuffer.h"
#include "VulkanDevice.h"
#include "VulkanRenderPass.h"
#include "VulkanFramebuffer.h"
#include "VulkanBuffer.h"
#include "VulkanPipeline.h"
#include "VulkanImageView.h"
#include "VulkanPipileneStates.h"
#include "VulkanDescriptors.h"
#include "Tasks/Scheduler.h"
#include "VulkanImage.h"
#include "Containers/Pair.h"
#include "Memory/RefPtr.hpp"

using namespace Sailor;
using namespace Sailor::GraphicsDriver::Vulkan;

VulkanCommandBuffer::VulkanCommandBuffer(VulkanDevicePtr device, VulkanCommandPoolPtr commandPool, VkCommandBufferLevel level) :
	m_device(device),
	m_level(level),
	m_commandPool(commandPool)
{
	VkCommandBufferAllocateInfo allocateInfo = {};
	allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocateInfo.commandPool = *commandPool;
	allocateInfo.level = level;
	allocateInfo.commandBufferCount = 1;

	VK_CHECK(vkAllocateCommandBuffers(*m_device, &allocateInfo, &m_commandBuffer));

	m_currentThreadId = GetCurrentThreadId();
}

VulkanCommandBuffer::~VulkanCommandBuffer()
{
	DWORD currentThreadId = GetCurrentThreadId();

	auto duplicatedDevice = m_device;
	auto duplicatedCommandPool = m_commandPool;
	auto duplicatedCommandBuffer = m_commandBuffer;

	auto pReleaseResource = Tasks::Scheduler::CreateTask("Release command buffer", [=]()
	{
		if (duplicatedCommandBuffer)
		{
			vkFreeCommandBuffers(*duplicatedDevice, *duplicatedCommandPool, 1, &duplicatedCommandBuffer);
		}
	});

	if (m_currentThreadId == currentThreadId)
	{
		pReleaseResource->Execute();
		m_device.Clear();
	}
	else
	{
		App::GetSubmodule<Tasks::Scheduler>()->Run(pReleaseResource, m_currentThreadId);
	}
	ClearDependencies();
}

VulkanCommandPoolPtr VulkanCommandBuffer::GetCommandPool() const
{
	return m_commandPool;
}

void VulkanCommandBuffer::BeginCommandList(VkCommandBufferUsageFlags flags)
{
	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = flags;
	beginInfo.pInheritanceInfo = nullptr;

	VK_CHECK(vkBeginCommandBuffer(m_commandBuffer, &beginInfo));

	ClearDependencies();
}

void VulkanCommandBuffer::BeginSecondaryCommandList(const TVector<VkFormat>& colorAttachments, VkFormat depthStencilAttachment, VkCommandBufferUsageFlags flags, VkRenderingFlags inheritanceFlags)
{
	ClearDependencies();

	VkCommandBufferInheritanceRenderingInfoKHR attachments{};
	attachments.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO;
	attachments.colorAttachmentCount = (uint32_t)colorAttachments.Num();
	attachments.depthAttachmentFormat = depthStencilAttachment;
	attachments.stencilAttachmentFormat = depthStencilAttachment;
	attachments.pColorAttachmentFormats = colorAttachments.GetData();

	// TODO: Should we always use MSAA?
	attachments.rasterizationSamples = m_device->GetCurrentMsaaSamples();

	VkCommandBufferInheritanceInfo inheritanceInfo{};
	inheritanceInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
	inheritanceInfo.renderPass = nullptr;
	inheritanceInfo.subpass = 0;
	inheritanceInfo.pNext = &attachments;

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = flags;
	beginInfo.pInheritanceInfo = &inheritanceInfo;

	VK_CHECK(vkBeginCommandBuffer(m_commandBuffer, &beginInfo));
}

void VulkanCommandBuffer::BeginSecondaryCommandList(VulkanRenderPassPtr renderPass, uint32_t subpassIndex, VkCommandBufferUsageFlags flags)
{
	ClearDependencies();

	// We can omit framebuffer for now
	VkCommandBufferInheritanceInfo inheritanceInfo{};
	inheritanceInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
	inheritanceInfo.renderPass = *renderPass;
	inheritanceInfo.subpass = subpassIndex;

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = flags;
	beginInfo.pInheritanceInfo = &inheritanceInfo;

	VK_CHECK(vkBeginCommandBuffer(m_commandBuffer, &beginInfo));

	m_renderPass = renderPass;
}

void VulkanCommandBuffer::CopyBuffer(VulkanBufferPtr src, VulkanBufferPtr dst, VkDeviceSize size, VkDeviceSize srcOffset, VkDeviceSize dstOffset)
{
	VkBufferCopy copyRegion{};
	copyRegion.srcOffset = srcOffset; // Optional
	copyRegion.dstOffset = dstOffset; // Optional
	copyRegion.size = size;

	vkCmdCopyBuffer(m_commandBuffer, *src, *dst, 1, &copyRegion);

	m_bufferDependencies.Add(src);
	m_bufferDependencies.Add(dst);
}

void VulkanCommandBuffer::CopyBufferToImage(VulkanBufferPtr src, VulkanImagePtr image, uint32_t width, uint32_t height, VkDeviceSize srcOffset)
{
	VkBufferImageCopy region{};
	region.bufferOffset = srcOffset;
	region.bufferRowLength = 0;
	region.bufferImageHeight = 0;

	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel = 0;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount = 1;

	region.imageOffset = { 0, 0, 0 };
	region.imageExtent = {
		width,
		height,
		1
	};

	vkCmdCopyBufferToImage(
		m_commandBuffer,
		*src,
		*image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1,
		&region
	);

	m_bufferDependencies.Add(src);
	m_imageDependencies.Add(image);
}

void VulkanCommandBuffer::EndCommandList()
{
	m_bIsRecorded = true;
	VK_CHECK(vkEndCommandBuffer(m_commandBuffer));
}

void VulkanCommandBuffer::BeginRenderPassEx(const TVector<VulkanImageViewPtr>& colorAttachments,
	VulkanImageViewPtr depthStencilAttachment,
	VkRect2D renderArea,
	VkRenderingFlags renderingFlags,
	VkOffset2D offset,
	VkClearValue clearColor)
{
	VkClearValue depthClear{};
	depthClear.depthStencil = VulkanApi::DefaultClearDepthStencilValue;

	const VkRenderingAttachmentInfoKHR depthStencilAttachmentInfo
	{
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = *depthStencilAttachment,
		.imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL_KHR,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.clearValue = depthClear,
	};

	const VkRenderingAttachmentInfoKHR colorAttachmentInfo
	{
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR,
		.imageView = *(colorAttachments[0]),
		.imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL_KHR,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.clearValue = clearColor,
	};

	const VkRenderingInfoKHR renderInfo
	{
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR,
		.pNext = VK_NULL_HANDLE,
		.flags = renderingFlags,
		.renderArea = renderArea,
		.layerCount = 1,
		.colorAttachmentCount = (uint32_t)colorAttachments.Num(),
		.pColorAttachments = &colorAttachmentInfo,
		.pDepthAttachment = &depthStencilAttachmentInfo,
		.pStencilAttachment = &depthStencilAttachmentInfo,
	};

	for (auto& attachment : colorAttachments)
	{
		m_colorAttachmentDependencies.Add(attachment->GetImage());
	}
	m_depthStencilAttachmentDependency = depthStencilAttachment->GetImage();

	m_device->vkCmdBeginRenderingKHR(m_commandBuffer, &renderInfo);
}

void VulkanCommandBuffer::EndRenderPassEx()
{
	m_device->vkCmdEndRenderingKHR(m_commandBuffer);
}

void VulkanCommandBuffer::BeginRenderPass(VulkanRenderPassPtr renderPass, VulkanFramebufferPtr frameBuffer, VkExtent2D extent, VkSubpassContents content, VkOffset2D offset, VkClearValue clearColor)
{
	m_renderPass = renderPass;

	VkRenderPassBeginInfo renderPassInfo{};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderPassInfo.renderPass = *renderPass;
	renderPassInfo.framebuffer = *frameBuffer;
	renderPassInfo.renderArea.offset = offset;
	renderPassInfo.renderArea.extent = extent;

	bool bMSSA = frameBuffer->GetAttachments().Num() == 3;

	std::array<VkClearValue, 3> clearValues;
	clearValues[0].color = clearColor.color;
	clearValues[1].color = clearColor.color;
	clearValues[2].depthStencil = VulkanApi::DefaultClearDepthStencilValue;

	renderPassInfo.clearValueCount = bMSSA ? 3U : 2U;
	renderPassInfo.pClearValues = bMSSA ? clearValues.data() : &clearValues.data()[1];

	vkCmdBeginRenderPass(m_commandBuffer, &renderPassInfo, content);
}

void VulkanCommandBuffer::SetDepthBias(float depthBiasConstantFactor, float depthBiasClamp, float depthBiasSlopeFactor)
{
	vkCmdSetDepthBias(m_commandBuffer, depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor);
}

void VulkanCommandBuffer::BindVertexBuffers(TVector<VulkanBufferPtr> buffers, TVector<VkDeviceSize> offsets, uint32_t firstBinding, uint32_t bindingCount)
{
	VkBuffer* vertexBuffers = reinterpret_cast<VkBuffer*>(_malloca(buffers.Num() * sizeof(VkBuffer)));

	for (int i = 0; i < buffers.Num(); i++)
	{
		vertexBuffers[i] = *buffers[i];
	}
	vkCmdBindVertexBuffers(m_commandBuffer, firstBinding, bindingCount, &vertexBuffers[0], &offsets[0]);
	m_bufferDependencies.AddRange(buffers);
	_freea(vertexBuffers);
}

void VulkanCommandBuffer::BindIndexBuffer(VulkanBufferPtr indexBuffer)
{
	m_bufferDependencies.Add(indexBuffer);
	vkCmdBindIndexBuffer(m_commandBuffer, *indexBuffer, 0, VK_INDEX_TYPE_UINT32);
}

void VulkanCommandBuffer::BindDescriptorSet(VulkanPipelineLayoutPtr pipelineLayout, TVector<VulkanDescriptorSetPtr> descriptorSet)
{
	VkDescriptorSet* sets = reinterpret_cast<VkDescriptorSet*>(_malloca(descriptorSet.Num() * sizeof(VkDescriptorSet)));

	for (int i = 0; i < descriptorSet.Num(); i++)
	{
		sets[i] = *descriptorSet[i];
		m_descriptorSetDependencies.Add(descriptorSet[i]);

	}
	vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, *pipelineLayout, 0, (uint32_t)descriptorSet.Num(), &sets[0], 0, nullptr);
	_freea(sets);
}

void VulkanCommandBuffer::BindPipeline(VulkanPipelinePtr pipeline)
{
	m_pipelineDependencies.Add(pipeline);
	vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, *pipeline);
}

void VulkanCommandBuffer::DrawIndexed(VulkanBufferPtr indexBuffer, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, uint32_t vertexOffset, uint32_t firstInstance)
{
	m_bufferDependencies.Add(indexBuffer);
	vkCmdDrawIndexed(m_commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void VulkanCommandBuffer::EndRenderPass()
{
	vkCmdEndRenderPass(m_commandBuffer);
}

void VulkanCommandBuffer::Reset()
{
	vkResetCommandBuffer(m_commandBuffer, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
	ClearDependencies();
}

void VulkanCommandBuffer::AddDependency(TMemoryPtr<VulkanBufferMemoryPtr> ptr, TWeakPtr<VulkanBufferAllocator> allocator)
{
	m_memoryPtrs.Add(TPair(ptr, allocator));
}

void VulkanCommandBuffer::AddDependency(VulkanSemaphorePtr semaphore)
{
	m_semaphoreDependencies.Add(semaphore);
}

void VulkanCommandBuffer::ClearDependencies()
{
	m_bufferDependencies.Clear();
	m_imageDependencies.Clear();
	m_descriptorSetDependencies.Clear();
	m_pipelineDependencies.Clear();
	m_semaphoreDependencies.Clear();
	m_commandBufferDependencies.Clear();
	m_renderPass.Clear();
	m_colorAttachmentDependencies.Clear();
	m_depthStencilAttachmentDependency.Clear();

	for (auto& managedPtr : m_memoryPtrs)
	{
		if (managedPtr.m_first && managedPtr.m_second)
		{
			managedPtr.m_second.Lock()->Free(managedPtr.m_first);
		}
	}

	m_memoryPtrs.Clear();
}

void VulkanCommandBuffer::Execute(VulkanCommandBufferPtr secondaryCommandBuffer)
{
	assert(secondaryCommandBuffer->IsRecorded());

	vkCmdExecuteCommands(m_commandBuffer, 1, secondaryCommandBuffer->GetHandle());
	m_commandBufferDependencies.Add(secondaryCommandBuffer);
}

void VulkanCommandBuffer::SetViewport(VulkanStateViewportPtr viewport)
{
	m_bHasViewport = true;
	m_cachedViewportSettings = viewport->GetViewport();

	vkCmdSetViewport(m_commandBuffer, 0, 1, &m_cachedViewportSettings);
}

bool VulkanCommandBuffer::FitsViewport(const VkViewport& viewport) const
{
	return !m_bHasViewport || std::memcmp(&m_cachedViewportSettings, &viewport, sizeof(VkViewport)) == 0;
}

void VulkanCommandBuffer::SetScissor(VulkanStateViewportPtr viewport)
{
	vkCmdSetScissor(m_commandBuffer, 0, 1, &viewport->GetScissor());
}

void VulkanCommandBuffer::Blit(VulkanImagePtr srcImage, VkImageLayout srcImageLayout, VulkanImagePtr dstImage, VkImageLayout dstImageLayout,
	uint32_t regionCount, const VkImageBlit* pRegions, VkFilter filter)
{
	vkCmdBlitImage(m_commandBuffer, *srcImage, srcImageLayout, *dstImage, dstImageLayout, regionCount, pRegions, filter);
}

void VulkanCommandBuffer::GenerateMipMaps(VulkanImagePtr image)
{
	if (!image->GetDevice()->IsMipsSupported(image->m_format))
	{
		SAILOR_LOG("Blit is not supported");
	}

	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.image = *image;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;
	barrier.subresourceRange.levelCount = 1;

	int32_t mipWidth = image->m_extent.width;
	int32_t mipHeight = image->m_extent.height;

	for (uint32_t i = 1; i < image->m_mipLevels; i++)
	{
		barrier.subresourceRange.baseMipLevel = i - 1;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

		vkCmdPipelineBarrier(m_commandBuffer,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
			0, nullptr,
			0, nullptr,
			1, &barrier);

		VkImageBlit blit{};
		blit.srcOffsets[0] = { 0, 0, 0 };
		blit.srcOffsets[1] = { mipWidth, mipHeight, 1 };
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.mipLevel = i - 1;
		blit.srcSubresource.baseArrayLayer = 0;
		blit.srcSubresource.layerCount = 1;
		blit.dstOffsets[0] = { 0, 0, 0 };
		blit.dstOffsets[1] = { mipWidth > 1 ? mipWidth / 2 : 1, mipHeight > 1 ? mipHeight / 2 : 1, 1 };
		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.mipLevel = i;
		blit.dstSubresource.baseArrayLayer = 0;
		blit.dstSubresource.layerCount = 1;

		Blit(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &blit,
			VK_FILTER_LINEAR);

		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		vkCmdPipelineBarrier(m_commandBuffer,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
			0, nullptr,
			0, nullptr,
			1, &barrier);

		if (mipWidth > 1)
		{
			mipWidth /= 2;
		}

		if (mipHeight > 1)
		{
			mipHeight /= 2;
		}
	}
	barrier.subresourceRange.baseMipLevel = image->m_mipLevels - 1;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	vkCmdPipelineBarrier(m_commandBuffer,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
		0, nullptr,
		0, nullptr,
		1, &barrier);
}

void VulkanCommandBuffer::ImageMemoryBarrier(VulkanImagePtr image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout)
{
	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = *image;
	barrier.subresourceRange.aspectMask = VulkanApi::ComputeAspectFlagsForFormat(format);
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = image->m_mipLevels;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = image->m_arrayLayers;
	barrier.srcAccessMask = 0; // TODO
	barrier.dstAccessMask = 0; // TODO

	VkPipelineStageFlags sourceStage;
	VkPipelineStageFlags destinationStage;

	if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
	{
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	{
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	}
	else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
	{
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	}
	else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
	{
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	}
	else if (oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
	{
		barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		sourceStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		destinationStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	}
	else
	{
		assert("Unsupported layout transition!");
	}

	vkCmdPipelineBarrier(
		m_commandBuffer,
		sourceStage, destinationStage,
		0,
		0, nullptr,
		0, nullptr,
		1, &barrier
	);

	m_imageDependencies.Add(image);
}
