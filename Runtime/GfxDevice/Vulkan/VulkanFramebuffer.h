#pragma once
#include "VulkanRenderPass.h"

using namespace Sailor;
namespace Sailor::GfxDevice::Vulkan
{
	class VulkanDevice;
	class VulkanImageView;

	class VulkanFramebuffer : public RHI::RHIResource
	{
	public:
		VulkanFramebuffer(TRefPtr<VulkanRenderPass> renderPass, const std::vector<TRefPtr<VulkanImageView>>& attachments, uint32_t width, uint32_t height, uint32_t layers);

		operator VkFramebuffer() const { return m_framebuffer; }

		TRefPtr<VulkanRenderPass> GetRenderPass() { return m_renderPass; }

		std::vector<TRefPtr<VulkanImageView>>& GetAttachments() { return m_attachments; }
		const std::vector<TRefPtr<VulkanImageView>>& GetAttachments() const { return m_attachments; }

		uint32_t GetWidth() const { return m_width; }
		uint32_t GetHeight() const { return m_height; }
		uint32_t GetLayers() const { return m_layers; }

	protected:
		virtual ~VulkanFramebuffer();

		VkFramebuffer m_framebuffer;
		TRefPtr<VulkanDevice> m_device;

		TRefPtr<VulkanRenderPass> m_renderPass;
		std::vector<TRefPtr<VulkanImageView>> m_attachments;

		const uint32_t m_width;
		const uint32_t m_height;
		const uint32_t m_layers;
	};
}
