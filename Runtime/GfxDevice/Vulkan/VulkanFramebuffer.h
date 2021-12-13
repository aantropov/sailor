#pragma once
#include "VulkanRenderPass.h"

using namespace Sailor;
namespace Sailor::GfxDevice::Vulkan
{
	class VulkanDevice;
	class VulkanImageView;

	class VulkanFramebuffer : public RHI::Resource
	{
	public:
		VulkanFramebuffer(VulkanRenderPassPtr renderPass, const TVector<VulkanImageViewPtr>& attachments, uint32_t width, uint32_t height, uint32_t layers);

		operator VkFramebuffer() const { return m_framebuffer; }

		VulkanRenderPassPtr GetRenderPass() { return m_renderPass; }

		TVector<VulkanImageViewPtr>& GetAttachments() { return m_attachments; }
		const TVector<VulkanImageViewPtr>& GetAttachments() const { return m_attachments; }

		uint32_t GetWidth() const { return m_width; }
		uint32_t GetHeight() const { return m_height; }
		uint32_t GetLayers() const { return m_layers; }

	protected:
		virtual ~VulkanFramebuffer();

		VkFramebuffer m_framebuffer;
		VulkanDevicePtr m_device;

		VulkanRenderPassPtr m_renderPass;
		TVector<VulkanImageViewPtr> m_attachments;

		const uint32_t m_width;
		const uint32_t m_height;
		const uint32_t m_layers;
	};
}
