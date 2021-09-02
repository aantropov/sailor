#include "VulkanFramebuffer.h"
#include "VulkanRenderPass.h"
#include "VulkanImageView.h"

using namespace Sailor;
using namespace Sailor::GfxDevice::Vulkan;

VulkanFramebuffer::VulkanFramebuffer(TRefPtr<VulkanRenderPass> renderPass, const std::vector<TRefPtr<VulkanImageView>>& attachments, uint32_t width, uint32_t height, uint32_t layers) :
	m_device(renderPass->GetDevice()),
	m_renderPass(renderPass),
	m_attachments(attachments),
	m_width(width),
	m_height(height),
	m_layers(layers)
{
	std::vector<VkImageView> vkAttachments;

	for (auto& attachment : attachments)
	{
		vkAttachments.push_back((VkImageView)*attachment);
	}

	VkFramebufferCreateInfo framebufferInfo = {};
	framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebufferInfo.flags = 0;
	framebufferInfo.renderPass = *m_renderPass;
	framebufferInfo.attachmentCount = static_cast<uint32_t>(vkAttachments.size());
	framebufferInfo.pAttachments = vkAttachments.data();
	framebufferInfo.width = width;
	framebufferInfo.height = height;
	framebufferInfo.layers = layers;

	VK_CHECK(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_framebuffer));
}

VulkanFramebuffer::~VulkanFramebuffer()
{
	if (m_framebuffer)
	{
		vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
	}
}
