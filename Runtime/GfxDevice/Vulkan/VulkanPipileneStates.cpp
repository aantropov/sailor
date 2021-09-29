#include "VulkanPipileneStates.h"
#include "Core/RefPtr.hpp"

using namespace Sailor;
using namespace Sailor::GfxDevice::Vulkan;

VulkanStateViewport::VulkanStateViewport(float width, float height) :
	VulkanStateViewport(0.0f, 0.0f, width, height, { 0,0 }, { (uint32_t)width, (uint32_t)height }, 0.0f, 1.0f)
{
}

VulkanStateViewport::VulkanStateViewport(float x, float y, float width, float height, VkOffset2D scissorOffset, VkExtent2D scissorExtent, float minDepth, float maxDepth)
	: m_viewport{}, m_scissor{}, m_viewportState{}
{
	m_viewport.x = x;
	m_viewport.y = y;
	m_viewport.width = width;
	m_viewport.height = height;
	m_viewport.minDepth = minDepth;
	m_viewport.maxDepth = maxDepth;

	m_scissor.extent = scissorExtent;
	m_scissor.offset = scissorOffset;

	m_viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	m_viewportState.viewportCount = 1;
	m_viewportState.pViewports = &m_viewport;
	m_viewportState.scissorCount = 1;
	m_viewportState.pScissors = &m_scissor;
}

void VulkanStateViewport::Apply(VkGraphicsPipelineCreateInfo& state) const
{
	state.pViewportState = &m_viewportState;
}

VulkanStateDynamicViewport::VulkanStateDynamicViewport() : m_viewportState{}
{
	m_viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	m_viewportState.viewportCount = 1;
	m_viewportState.pViewports = nullptr;
	m_viewportState.scissorCount = 1;
	m_viewportState.pScissors = nullptr;
}

void VulkanStateDynamicViewport::Apply(VkGraphicsPipelineCreateInfo& state) const
{
	state.pViewportState = &m_viewportState;
}

VulkanStateVertexDescription::VulkanStateVertexDescription(const VkVertexInputBindingDescription& binding, const std::vector<VkVertexInputAttributeDescription> attributes) :
	m_bindingDescription(binding), m_attributeDescriptions(std::move(attributes)), m_vertexInput{}
{
	m_vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	m_vertexInput.vertexBindingDescriptionCount = 1;
	m_vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(m_attributeDescriptions.size());
	m_vertexInput.pVertexBindingDescriptions = &m_bindingDescription;
	m_vertexInput.pVertexAttributeDescriptions = m_attributeDescriptions.data();
}

void VulkanStateVertexDescription::Apply(VkGraphicsPipelineCreateInfo& state) const
{
	state.pVertexInputState = &m_vertexInput;
}

VulkanStateInputAssembly::VulkanStateInputAssembly(VkPrimitiveTopology topology, bool bPrimitiveRestartEnable) :
	m_inputAssembly{}

{
	m_inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	m_inputAssembly.topology = topology;
	m_inputAssembly.primitiveRestartEnable = bPrimitiveRestartEnable;
}

void VulkanStateInputAssembly::Apply(struct VkGraphicsPipelineCreateInfo& state) const
{
	state.pInputAssemblyState = &m_inputAssembly;
}

VulkanStateRasterization::VulkanStateRasterization() :
	m_rasterizer{}
{
	m_rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	m_rasterizer.depthClampEnable = VK_FALSE;
	m_rasterizer.rasterizerDiscardEnable = VK_FALSE;
	m_rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
	m_rasterizer.lineWidth = 1.0f;
	m_rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
	m_rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

	m_rasterizer.depthBiasEnable = VK_FALSE;
	m_rasterizer.depthBiasConstantFactor = 0.0f; // Optional
	m_rasterizer.depthBiasClamp = 0.0f; // Optional
	m_rasterizer.depthBiasSlopeFactor = 0.0f; // Optional
}

void VulkanStateRasterization::Apply(struct VkGraphicsPipelineCreateInfo& state) const
{
	state.pRasterizationState = &m_rasterizer;
}

VulkanStateMultisample::VulkanStateMultisample(VkSampleCountFlagBits samples) : m_multisampling{}
{
	m_multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	m_multisampling.sampleShadingEnable = VK_FALSE;
	m_multisampling.rasterizationSamples = samples;
	m_multisampling.minSampleShading = 1.0f; // Optional
	m_multisampling.pSampleMask = nullptr; // Optional
	m_multisampling.alphaToCoverageEnable = VK_FALSE; // Optional
	m_multisampling.alphaToOneEnable = VK_FALSE; // Optional

#ifdef SAILOR_MSAA_IMPACTS_TEXTURE_SAMPLING
	m_multisampling.sampleShadingEnable = VK_TRUE; 
	m_multisampling.minSampleShading = 0.2f;
#endif

}

void VulkanStateMultisample::Apply(struct VkGraphicsPipelineCreateInfo& state) const
{
	state.pMultisampleState = &m_multisampling;
}

VulkanStateColorBlending::VulkanStateColorBlending(bool blendEnable, VkBlendFactor srcColorBlendFactor,
	VkBlendFactor dstColorBlendFactor, VkBlendOp colorBlendOp, VkBlendFactor srcAlphaBlendFactor,
	VkBlendFactor dstAlphaBlendFactor, VkBlendOp alphaBlendOp, VkColorComponentFlags colorWriteMask) :
	m_colorBlendAttachment{}, m_colorBlending{}
{
	m_colorBlendAttachment.colorWriteMask = colorWriteMask;
	m_colorBlendAttachment.blendEnable = blendEnable;
	m_colorBlendAttachment.srcColorBlendFactor = srcColorBlendFactor; // Optional
	m_colorBlendAttachment.dstColorBlendFactor = dstColorBlendFactor; // Optional
	m_colorBlendAttachment.colorBlendOp = colorBlendOp; // Optional
	m_colorBlendAttachment.srcAlphaBlendFactor = srcAlphaBlendFactor; // Optional
	m_colorBlendAttachment.dstAlphaBlendFactor = dstAlphaBlendFactor; // Optional
	m_colorBlendAttachment.alphaBlendOp = alphaBlendOp; // Optional

	m_colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	m_colorBlending.logicOpEnable = VK_FALSE;
	m_colorBlending.logicOp = VK_LOGIC_OP_COPY; // Optional
	m_colorBlending.attachmentCount = 1;
	m_colorBlending.pAttachments = &m_colorBlendAttachment;
	m_colorBlending.blendConstants[0] = 0.0f; // Optional
	m_colorBlending.blendConstants[1] = 0.0f; // Optional
	m_colorBlending.blendConstants[2] = 0.0f; // Optional
	m_colorBlending.blendConstants[3] = 0.0f; // Optional
}

void VulkanStateColorBlending::Apply(struct VkGraphicsPipelineCreateInfo& state) const
{
	state.pColorBlendState = &m_colorBlending;
}

VulkanStateDynamic::VulkanStateDynamic() : m_dynamicState{}
{
	m_dynamicStates.push_back(VK_DYNAMIC_STATE_VIEWPORT);
	m_dynamicStates.push_back(VK_DYNAMIC_STATE_SCISSOR);
	
	m_dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	m_dynamicState.dynamicStateCount = 2;
	m_dynamicState.pDynamicStates = m_dynamicStates.data();
}

void VulkanStateDynamic::Apply(struct VkGraphicsPipelineCreateInfo& state) const
{
	state.pDynamicState = &m_dynamicState;
}

VulkanStateDepthStencil::VulkanStateDepthStencil(bool bEnableDepthTest, bool bEnableZWrite, VkCompareOp depthOp)
{
	m_depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	m_depthStencil.depthTestEnable = bEnableDepthTest;
	m_depthStencil.depthWriteEnable = bEnableZWrite;
	m_depthStencil.depthCompareOp = depthOp;

	// Min depth -> closier
	m_depthStencil.depthBoundsTestEnable = VK_FALSE;
	m_depthStencil.minDepthBounds = 0.0f; // Optional
	m_depthStencil.maxDepthBounds = 1.0f; // Optional
}

void VulkanStateDepthStencil::Apply(struct VkGraphicsPipelineCreateInfo& state) const
{

	state.pDepthStencilState = &m_depthStencil;
}

