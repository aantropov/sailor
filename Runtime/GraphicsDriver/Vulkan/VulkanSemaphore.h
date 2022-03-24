#pragma once
#include "VulkanApi.h"
#include "VulkanDevice.h"
#include "Memory/RefPtr.hpp"
#include "RHI/Types.h"

namespace Sailor::GraphicsDriver::Vulkan
{
	class VulkanSemaphore final : public RHI::Resource
	{

	public:

		SAILOR_API VulkanSemaphore(VulkanDevicePtr device, VkPipelineStageFlags pipelineStageFlags = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, void* pNextCreateInfo = nullptr);

		SAILOR_API operator VkSemaphore() const { return m_semaphore; }

		SAILOR_API VkPipelineStageFlags& PipelineStageFlags() { return m_pipelineStageFlags; }
		SAILOR_API const VkPipelineStageFlags& PipelineStageFlags() const { return m_pipelineStageFlags; }
		SAILOR_API const VkSemaphore* GetHandle() const { return &m_semaphore; }

	protected:
		SAILOR_API virtual ~VulkanSemaphore();

		VkSemaphore m_semaphore;
		VkPipelineStageFlags m_pipelineStageFlags = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

		VulkanDevicePtr m_device;
	};
}
