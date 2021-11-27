#pragma once
#include <vector>
#include <unordered_map>
#include "vulkan/vulkan.h"
#include "Memory/RefPtr.hpp"
#include "RHI/Types.h"

namespace Sailor::GfxDevice::Vulkan
{
	typedef TRefPtr<class VulkanDevice> VulkanDevicePtr;
	typedef TRefPtr<class VulkanPipelineState> VulkanPipelineStatePtr;
	typedef TRefPtr<class VulkanStateColorBlending> VulkanStateColorBlendingPtr;

	class VulkanPipelineState : public RHI::Resource, public RHI::IStateModifier<VkGraphicsPipelineCreateInfo> {};

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

		VulkanStateRasterization(bool bEnableDepthBias = false, float depthBias = 0.0f, VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT, VkPolygonMode fillMode = VK_POLYGON_MODE_FILL);
		void Apply(struct VkGraphicsPipelineCreateInfo& state) const override;

	private:
		VkPipelineRasterizationStateCreateInfo m_rasterizer;
	};

	class VulkanStateMultisample : public VulkanPipelineState
	{
	public:

		VulkanStateMultisample(VkSampleCountFlagBits samples);
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
		VulkanStateDepthStencil(bool bEnableDepthTest = true, bool bEnableZWrite = true, VkCompareOp depthOp = VkCompareOp::VK_COMPARE_OP_LESS);
		void Apply(struct VkGraphicsPipelineCreateInfo& state) const override;

	private:

		VkPipelineDepthStencilStateCreateInfo m_depthStencil{};
	};

	struct VulkanPipelineStateBuilder
	{
		VulkanPipelineStateBuilder(VulkanDevicePtr pDevice);
		const std::vector<VulkanPipelineStatePtr>& BuildPipeline(const RHI::RenderState& renderState);

	protected:

		VulkanStateColorBlendingPtr GetBlendState(RHI::EBlendMode mode);

		VulkanDevicePtr m_pDevice;
		std::vector<VulkanStateColorBlendingPtr> m_blendModes;
		std::unordered_map<RHI::RenderState, std::vector<VulkanPipelineStatePtr>> m_cache;
	};
}