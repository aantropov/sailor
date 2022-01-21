#include "VulkanFramebuffer.h"
#include "VulkanRenderPass.h"
#include "VulkanImageView.h"
#include "VulkanDevice.h"

using namespace Sailor;
using namespace Sailor::GraphicsDriver::Vulkan;

VulkanFramebuffer::VulkanFramebuffer(VulkanRenderPassPtr renderPass, const TVector<VulkanImageViewPtr>& attachments, uint32_t width, uint32_t height, uint32_t layers) :
	m_device(renderPass->GetDevice()),
	m_renderPass(renderPass),
	m_attachments(attachments),
	m_width(width),
	m_height(height),
	m_layers(layers)
{
	TVector<VkImageView> vkAttachments;

	for (auto& attachment : attachments)
	{
		vkAttachments.Add((VkImageView)*attachment);
	}

	VkFramebufferCreateInfo framebufferInfo = {};
	framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebufferInfo.flags = 0;
	framebufferInfo.renderPass = *m_renderPass;
	framebufferInfo.attachmentCount = static_cast<uint32_t>(vkAttachments.Num());
	framebufferInfo.pAttachments = vkAttachments.GetData();
	framebufferInfo.width = width;
	framebufferInfo.height = height;
	framebufferInfo.layers = layers;

	VK_CHECK(vkCreateFramebuffer(*m_device, &framebufferInfo, nullptr, &m_framebuffer));
}

VulkanFramebuffer::~VulkanFramebuffer()
{
	if (m_framebuffer)
	{
		vkDestroyFramebuffer(*m_device, m_framebuffer, nullptr);
	}
}
