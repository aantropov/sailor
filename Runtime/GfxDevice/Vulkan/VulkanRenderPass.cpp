#include "VulkanRenderPass.h"
#include "VulkanFramebuffer.h"
#include "VulkanDevice.h"
#include "Core/RefPtr.hpp"

using namespace Sailor::GfxDevice::Vulkan;

VulkanRenderPass::VulkanRenderPass(VulkanDevicePtr device,
	const std::vector<VkAttachmentDescription>& attachments,
	const std::vector<VulkanSubpassDescription>& subpasses,
	const std::vector<VkSubpassDependency>& dependencies) :
	m_device(device)
{
	m_maxMsSamples = VK_SAMPLE_COUNT_1_BIT;
	for (auto& attachment : attachments)
	{
		if (attachment.samples > m_maxMsSamples) m_maxMsSamples = attachment.samples;
	}

	std::vector<VkSubpassDescription> vk_subpasses(subpasses.size());
	for (size_t i = 0; i < subpasses.size(); ++i)
	{
		auto& src = subpasses[i];
		VkSubpassDescription& dst = vk_subpasses[i];
		dst.flags = src.m_flags;
		dst.pipelineBindPoint = src.m_pipelineBindPoint;
		dst.inputAttachmentCount = static_cast<uint32_t>(src.m_inputAttachments.size());
		dst.pInputAttachments = src.m_inputAttachments.empty() ? nullptr : src.m_inputAttachments.data();
		dst.colorAttachmentCount = static_cast<uint32_t>(src.m_colorAttachments.size());
		dst.pColorAttachments = src.m_colorAttachments.empty() ? nullptr : src.m_colorAttachments.data();
		dst.pResolveAttachments = src.m_depthStencilAttachments.empty() ? nullptr : src.m_resolveAttachments.data();
		dst.pDepthStencilAttachment = (src.m_depthStencilAttachments.empty()) ? nullptr : src.m_depthStencilAttachments.data();
		dst.preserveAttachmentCount = static_cast<uint32_t>(src.m_preserveAttachments.size());
		dst.pPreserveAttachments = src.m_preserveAttachments.empty() ? nullptr : src.m_preserveAttachments.data();
	}

	VkRenderPassCreateInfo renderPassInfo = {};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
	renderPassInfo.pAttachments = attachments.data();
	renderPassInfo.subpassCount = static_cast<uint32_t>(vk_subpasses.size());
	renderPassInfo.pSubpasses = vk_subpasses.data();
	renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
	renderPassInfo.pDependencies = dependencies.data();

	VK_CHECK(vkCreateRenderPass(*m_device, &renderPassInfo, nullptr, &m_renderPass));
}

VulkanDevicePtr VulkanRenderPass::GetDevice() const 
{
	return m_device; 
}

VulkanRenderPass::~VulkanRenderPass()
{
	if (m_renderPass)
	{
		vkDestroyRenderPass(*m_device, m_renderPass, nullptr);
	}
}
