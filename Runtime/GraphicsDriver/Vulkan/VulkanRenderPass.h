#pragma once
#include "VulkanApi.h"
#include "Memory/RefPtr.hpp"
#include "RHI/Types.h"

namespace Sailor::GraphicsDriver::Vulkan
{
	class VulkanFramebuffer;

	struct VulkanSubpassDescription
	{
		VkSubpassDescriptionFlags m_flags = 0;
		VkPipelineBindPoint m_pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		TVector<VkAttachmentReference> m_inputAttachments;
		TVector<VkAttachmentReference> m_colorAttachments;
		TVector<VkAttachmentReference> m_resolveAttachments;
		TVector<VkAttachmentReference> m_depthStencilAttachments;
		TVector<uint32_t> m_preserveAttachments;
	};

	class VulkanRenderPass final : public RHI::Resource
	{
	public:

		VulkanRenderPass(VulkanDevicePtr device,
			const TVector<VkAttachmentDescription>& attachments,
			const TVector<VulkanSubpassDescription>& subpasses,
			const TVector<VkSubpassDependency>& dependencies);

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
