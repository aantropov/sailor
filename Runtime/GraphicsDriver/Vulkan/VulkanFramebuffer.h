#pragma once
#include "VulkanRenderPass.h"

using namespace Sailor;
namespace Sailor::GraphicsDriver::Vulkan
{
	class VulkanDevice;
	class VulkanImageView;

	class VulkanFramebuffer : public RHI::RHIResource
	{
	public:
		SAILOR_API VulkanFramebuffer(VulkanRenderPassPtr renderPass, const TVector<VulkanImageViewPtr>& attachments, uint32_t width, uint32_t height, uint32_t layers);

		SAILOR_API operator VkFramebuffer() const { return m_framebuffer; }

		SAILOR_API VulkanRenderPassPtr GetRenderPass() { return m_renderPass; }

		SAILOR_API TVector<VulkanImageViewPtr>& GetAttachments() { return m_attachments; }
		SAILOR_API const TVector<VulkanImageViewPtr>& GetAttachments() const { return m_attachments; }

		SAILOR_API uint32_t GetWidth() const { return m_width; }
		SAILOR_API uint32_t GetHeight() const { return m_height; }
		SAILOR_API uint32_t GetLayers() const { return m_layers; }

	protected:
		SAILOR_API virtual ~VulkanFramebuffer();

		VkFramebuffer m_framebuffer;
		VulkanDevicePtr m_device;

		VulkanRenderPassPtr m_renderPass;
		TVector<VulkanImageViewPtr> m_attachments;

		const uint32_t m_width;
		const uint32_t m_height;
		const uint32_t m_layers;
	};
}
