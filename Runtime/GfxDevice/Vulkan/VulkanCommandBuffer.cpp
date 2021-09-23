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

void VulkanCommandBuffer::CopyBuffer(TRefPtr<VulkanBuffer>  src, TRefPtr<VulkanBuffer> dst, VkDeviceSize size, VkDeviceSize srcOffset, VkDeviceSize dstOffset)
{
	VkBufferCopy copyRegion{};
	copyRegion.srcOffset = 0; // Optional
	copyRegion.dstOffset = 0; // Optional
	copyRegion.size = size;
	vkCmdCopyBuffer(m_commandBuffer, *src, *dst, 1, &copyRegion);
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

	renderPassInfo.clearValueCount = 1;
	renderPassInfo.pClearValues = &clearColor;

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

	_freea(vertexBuffers);
}

void VulkanCommandBuffer::BindIndexBuffer(TRefPtr<VulkanBuffer> indexBuffer)
{
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
	vkCmdDrawIndexed(m_commandBuffer, (uint32_t)indexBuffer->m_size / sizeof(uint32_t), 1, 0, 0, 0);
}

void VulkanCommandBuffer::EndRenderPass()
{
	vkCmdEndRenderPass(m_commandBuffer);
}

void VulkanCommandBuffer::Reset()
{
	vkResetCommandBuffer(m_commandBuffer, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
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
