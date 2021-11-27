#pragma once
#include "VulkanApi.h"
#include "Memory/RefPtr.hpp"
#include "RHI/Types.h"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanFramebuffer;

	struct VulkanSubpassDescription
	{
		VkSubpassDescriptionFlags m_flags = 0;
		VkPipelineBindPoint m_pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		std::vector<VkAttachmentReference> m_inputAttachments;
		std::vector<VkAttachmentReference> m_colorAttachments;
		std::vector<VkAttachmentReference> m_resolveAttachments;
		std::vector<VkAttachmentReference> m_depthStencilAttachments;
		std::vector<uint32_t> m_preserveAttachments;
	};

	class VulkanRenderPass final : public RHI::Resource
	{
	public:

		VulkanRenderPass(VulkanDevicePtr device,
			const std::vector<VkAttachmentDescription>& attachments,
			const std::vector<VulkanSubpassDescription>& subpasses,
			const std::vector<VkSubpassDependency>& dependencies);

		operator VkRenderPass() const { return m_renderPass; }

		VulkanDevicePtr GetDevice() const;
		VkSampleCountFlagBits GetMaxMSSamples() const { return m_maxMsSamples; }

	protected:

		virtual ~VulkanRenderPass();

		VkRenderPass m_renderPass;
		VkSampleCountFlagBits m_maxMsSamples;
		VulkanDevicePtr m_device;
	};
}
