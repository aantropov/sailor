#pragma once
#include <vector>
#include "vulkan/vulkan.h"
#include "Core/RefPtr.hpp"
#include "RHI/RHIResource.h"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanPipelineState : public RHI::RHIResource, public RHI::IRHIStateModifier<VkGraphicsPipelineCreateInfo> {};

	class VulkanStateViewport : public VulkanPipelineState
	{
	public:

		VulkanStateViewport(float width, float height);
		VulkanStateViewport(float x, float y, float width, float height, VkOffset2D scissorOffset, VkExtent2D scissorExtent, float minDepth = 0.0f, float maxDepth = 1.0f);

		void Apply(struct VkGraphicsPipelineCreateInfo& state) const override;

		const VkViewport& GetViewport() const { return m_viewport; }
		const VkRect2D& GetScissor() const { return m_scissor; }

	private:

		VkViewport m_viewport;
		VkRect2D m_scissor;
		VkPipelineViewportStateCreateInfo m_viewportState;
	};

	class VulkanStateDynamicViewport : public VulkanPipelineState
	{
	public:
		VulkanStateDynamicViewport();

		void Apply(struct VkGraphicsPipelineCreateInfo& state) const override;
	private:

		VkPipelineViewportStateCreateInfo m_viewportState;
	};

	class VulkanStateVertexDescription : public VulkanPipelineState
	{
	public:

		VulkanStateVertexDescription(const VkVertexInputBindingDescription& binding, const std::vector<VkVertexInputAttributeDescription> attributes);

		void Apply(struct VkGraphicsPipelineCreateInfo& state) const override;

	private:

		VkVertexInputBindingDescription m_bindingDescription;
		std::vector<VkVertexInputAttributeDescription> m_attributeDescriptions;
		VkPipelineVertexInputStateCreateInfo m_vertexInput;
	};

	class VulkanStateInputAssembly : public VulkanPipelineState
	{
	public:

		VulkanStateInputAssembly(VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, bool bPrimitiveRestartEnable = false);
		void Apply(struct VkGraphicsPipelineCreateInfo& state) const override;

	private:

		VkPipelineInputAssemblyStateCreateInfo m_inputAssembly;
	};

	class VulkanStateRasterization : public VulkanPipelineState
	{
	public:

		VulkanStateRasterization();
		void Apply(struct VkGraphicsPipelineCreateInfo& state) const override;

	private:
		VkPipelineRasterizationStateCreateInfo m_rasterizer;
	};

	class VulkanStateMultisample : public VulkanPipelineState
	{
	public:

		VulkanStateMultisample();
		void Apply(struct VkGraphicsPipelineCreateInfo& state) const override;

	private:
		VkPipelineMultisampleStateCreateInfo m_multisampling;
	};

	class VulkanStateColorBlending : public VulkanPipelineState
	{
	public:
		VulkanStateColorBlending(bool blendEnable,
			VkBlendFactor            srcColorBlendFactor,
			VkBlendFactor            dstColorBlendFactor,
			VkBlendOp                colorBlendOp,
			VkBlendFactor            srcAlphaBlendFactor,
			VkBlendFactor            dstAlphaBlendFactor,
			VkBlendOp                alphaBlendOp,
			VkColorComponentFlags    colorWriteMask);

		void Apply(struct VkGraphicsPipelineCreateInfo& state) const override;

	private:

		VkPipelineColorBlendAttachmentState m_colorBlendAttachment;
		VkPipelineColorBlendStateCreateInfo m_colorBlending;
	};

	class VulkanStateDynamic : public VulkanPipelineState
	{
	public:
		VulkanStateDynamic();
		void Apply(struct VkGraphicsPipelineCreateInfo& state) const override;

	private:

		VkPipelineDynamicStateCreateInfo m_dynamicState;
		std::vector<VkDynamicState> m_dynamicStates;
	};

	class VulkanStateDepthStencil : public VulkanPipelineState
	{
	public:
		VulkanStateDepthStencil() = default;
		void Apply(struct VkGraphicsPipelineCreateInfo& state) const override;

	private:
	};
}