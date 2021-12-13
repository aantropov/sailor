#include "VulkanRenderPass.h"
#include "VulkanFramebuffer.h"
#include "VulkanDevice.h"
#include "Memory/RefPtr.hpp"

using namespace Sailor::GfxDevice::Vulkan;

VulkanRenderPass::VulkanRenderPass(VulkanDevicePtr device,
	const TVector<VkAttachmentDescription>& attachments,
	const TVector<VulkanSubpassDescription>& subpasses,
	const TVector<VkSubpassDependency>& dependencies) :
	m_device(device)
{
	m_maxMsSamples = VK_SAMPLE_COUNT_1_BIT;
	for (auto& attachment : attachments)
	{
		if (attachment.samples > m_maxMsSamples) m_maxMsSamples = attachment.samples;
	}

	TVector<VkSubpassDescription> vk_subpasses(subpasses.Num());
	for (size_t i = 0; i < subpasses.Num(); ++i)
	{
		auto& src = subpasses[i];
		VkSubpassDescription& dst = vk_subpasses[i];
		dst.flags = src.m_flags;
		dst.pipelineBindPoint = src.m_pipelineBindPoint;
		dst.inputAttachmentCount = static_cast<uint32_t>(src.m_inputAttachments.Num());
		dst.pInputAttachments = src.m_inputAttachments.IsEmpty() ? nullptr : src.m_inputAttachments.GetData();
		dst.colorAttachmentCount = static_cast<uint32_t>(src.m_colorAttachments.Num());
		dst.pColorAttachments = src.m_colorAttachments.IsEmpty() ? nullptr : src.m_colorAttachments.GetData();
		dst.pResolveAttachments = src.m_depthStencilAttachments.IsEmpty() ? nullptr : src.m_resolveAttachments.GetData();
		dst.pDepthStencilAttachment = (src.m_depthStencilAttachments.IsEmpty()) ? nullptr : src.m_depthStencilAttachments.GetData();
		dst.preserveAttachmentCount = static_cast<uint32_t>(src.m_preserveAttachments.Num());
		dst.pPreserveAttachments = src.m_preserveAttachments.IsEmpty() ? nullptr : src.m_preserveAttachments.GetData();
	}

	VkRenderPassCreateInfo renderPassInfo = {};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.Num());
	renderPassInfo.pAttachments = attachments.GetData();
	renderPassInfo.subpassCount = static_cast<uint32_t>(vk_subpasses.Num());
	renderPassInfo.pSubpasses = vk_subpasses.GetData();
	renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.Num());
	renderPassInfo.pDependencies = dependencies.GetData();

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
