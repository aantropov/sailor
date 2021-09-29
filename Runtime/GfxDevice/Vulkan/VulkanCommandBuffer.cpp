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
#include "VulkanPipileneStates.h"
#include "VulkanDescriptors.h"
#include "VulkanImage.h"
#include "Core/RefPtr.hpp"

using namespace Sailor;
using namespace Sailor::GfxDevice::Vulkan;

VulkanCommandBuffer::VulkanCommandBuffer(TRefPtr<VulkanDevice> device, Sailor::TRefPtr<VulkanCommandPool> commandPool, VkCommandBufferLevel level) :
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
}

VulkanCommandBuffer::~VulkanCommandBuffer()
{
	if (m_commandBuffer)
	{
		vkFreeCommandBuffers(*m_device, *m_commandPool, 1, &m_commandBuffer);
	}

	m_device.Clear();
}

TRefPtr<VulkanCommandPool> VulkanCommandBuffer::GetCommandPool() const
{
	return m_commandPool;
}

void VulkanCommandBuffer::BeginCommandList(VkCommandBufferUsageFlags flags)
{
	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = flags;

	VK_CHECK(vkBeginCommandBuffer(m_commandBuffer, &beginInfo));
}

void VulkanCommandBuffer::CopyBuffer(TRefPtr<VulkanBuffer> src, TRefPtr<VulkanBuffer> dst, VkDeviceSize size, VkDeviceSize srcOffset, VkDeviceSize dstOffset)
{
	VkBufferCopy copyRegion{};
	copyRegion.srcOffset = 0; // Optional
	copyRegion.dstOffset = 0; // Optional
	copyRegion.size = size;
	vkCmdCopyBuffer(m_commandBuffer, *src, *dst, 1, &copyRegion);

	m_bufferDependencies.push_back(src);
	m_bufferDependencies.push_back(dst);
}

void VulkanCommandBuffer::CopyBufferToImage(TRefPtr<VulkanBuffer> src, TRefPtr<VulkanImage> image, uint32_t width, uint32_t height)
{
	VkBufferImageCopy region{};
	region.bufferOffset = 0;
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

	m_bufferDependencies.push_back(src);
	m_imageDependencies.push_back(image);
}

void VulkanCommandBuffer::EndCommandList()
{
	VK_CHECK(vkEndCommandBuffer(m_commandBuffer));
}

void VulkanCommandBuffer::BeginRenderPass(TRefPtr<VulkanRenderPass> renderPass, TRefPtr<VulkanFramebuffer> frameBuffer, VkExtent2D extent, VkOffset2D offset, VkClearValue clearColor)
{
	VkRenderPassBeginInfo renderPassInfo{};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderPassInfo.renderPass = *renderPass;
	renderPassInfo.framebuffer = *frameBuffer;
	renderPassInfo.renderArea.offset = offset;
	renderPassInfo.renderArea.extent = extent;

	std::array<VkClearValue, 2> clearValues;
	clearValues[0].color = clearColor.color;
	clearValues[1].depthStencil = VulkanApi::DefaultClearDepthStencilValue;

	renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
	renderPassInfo.pClearValues = clearValues.data();

	vkCmdBeginRenderPass(m_commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
}

void VulkanCommandBuffer::BindVertexBuffers(std::vector<TRefPtr<VulkanBuffer>> buffers, std::vector<VkDeviceSize> offsets, uint32_t firstBinding, uint32_t bindingCount)
{
	VkBuffer* vertexBuffers = reinterpret_cast<VkBuffer*>(_malloca(buffers.size() * sizeof(VkBuffer)));

	for (int i = 0; i < buffers.size(); i++)
	{
		vertexBuffers[i] = *buffers[i];
	}
	vkCmdBindVertexBuffers(m_commandBuffer, firstBinding, bindingCount, &vertexBuffers[0], &offsets[0]);
	m_bufferDependencies.insert(m_bufferDependencies.end(), std::make_move_iterator(buffers.begin()), std::make_move_iterator(buffers.end()));
	_freea(vertexBuffers);
}

void VulkanCommandBuffer::BindIndexBuffer(TRefPtr<VulkanBuffer> indexBuffer)
{
	m_bufferDependencies.push_back(indexBuffer);
	vkCmdBindIndexBuffer(m_commandBuffer, *indexBuffer, 0, VK_INDEX_TYPE_UINT32);
}

void VulkanCommandBuffer::BindDescriptorSet(TRefPtr<VulkanPipelineLayout> pipelineLayout, TRefPtr<VulkanDescriptorSet> descriptorSet)
{
	vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, *pipelineLayout, 0, 1, descriptorSet->GetHandle(), 0, nullptr);
}

void VulkanCommandBuffer::BindPipeline(TRefPtr<VulkanPipeline> pipeline)
{
	vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, *pipeline);
}

void VulkanCommandBuffer::DrawIndexed(TRefPtr<VulkanBuffer> indexBuffer)
{
	m_bufferDependencies.push_back(indexBuffer);
	vkCmdDrawIndexed(m_commandBuffer, (uint32_t)indexBuffer->m_size / sizeof(uint32_t), 1, 0, 0, 0);
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

void VulkanCommandBuffer::ClearDependencies()
{
	m_bufferDependencies.clear();
	m_imageDependencies.clear();
}

void VulkanCommandBuffer::Execute(TRefPtr<VulkanCommandBuffer> secondaryCommandBuffer)
{
	vkCmdExecuteCommands(m_commandBuffer, 1, secondaryCommandBuffer->GetHandle());
}

void VulkanCommandBuffer::SetViewport(TRefPtr<const VulkanStateViewport> viewport)
{
	vkCmdSetViewport(m_commandBuffer, 0, 1, &viewport->GetViewport());
}

void VulkanCommandBuffer::SetScissor(TRefPtr<const VulkanStateViewport> viewport)
{
	vkCmdSetScissor(m_commandBuffer, 0, 1, &viewport->GetScissor());
}

void VulkanCommandBuffer::Blit(TRefPtr<VulkanImage> srcImage, VkImageLayout srcImageLayout, TRefPtr<VulkanImage> dstImage, VkImageLayout dstImageLayout,
	uint32_t regionCount, const VkImageBlit* pRegions, VkFilter filter)
{
}

void VulkanCommandBuffer::GenerateMipMaps(TRefPtr<VulkanImage> image)
{
	if (image->GetDevice()->IsMipsSupported(image->m_format))
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

void VulkanCommandBuffer::ImageMemoryBarrier(TRefPtr<VulkanImage> image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout)
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

	if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
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

	m_imageDependencies.push_back(image);
}
