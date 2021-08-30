#pragma once
#include "RHI/RHIResource.h"
#include "VulkanApi.h"

using namespace Sailor::RHI;

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanCommandPool;

	class VulkanCommandBuffer : public Sailor::RHI::RHIResource
	{

	public:

		const VkCommandBuffer* GetHandle() const { return &m_commandBuffer; }
		operator VkCommandBuffer() const { return m_commandBuffer; }

		VulkanCommandBuffer(VkDevice device, class TRefPtr<VulkanCommandPool> commandPool, VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY);

		virtual ~VulkanCommandBuffer() override;

	protected:

		VkDevice m_device;
		TRefPtr <VulkanCommandPool> m_commandPool;
		VkCommandBuffer m_commandBuffer;
		VkCommandBufferLevel m_level;
	};
}