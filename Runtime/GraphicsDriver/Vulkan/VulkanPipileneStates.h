#pragma once
#include "Containers/Vector.h"
#include "Containers/ConcurrentMap.h"
#include "vulkan/vulkan.h"
#include "Memory/RefPtr.hpp"
#include "RHI/Types.h"
#include "RHI/Renderer.h"

namespace Sailor::GraphicsDriver::Vulkan
{
	typedef TRefPtr<class VulkanDevice> VulkanDevicePtr;
	typedef TRefPtr<class VulkanPipelineState> VulkanPipelineStatePtr;
	typedef TRefPtr<class VulkanStateColorBlending> VulkanStateColorBlendingPtr;

	class VulkanPipelineState : public RHI::RHIResource, public RHI::IStateModifier<VkGraphicsPipelineCreateInfo> {};

	// This state allows to use VK_KHR_dynamic_rendering
	// and avoid the needs to create RenderPass & FrameBuffer
	class VulkanStateDynamicRendering : public VulkanPipelineState
	{
	public:
		SAILOR_API VulkanStateDynamicRendering(const TVector<VkFormat>& colorAttachments, VkFormat depthAttachment, VkFormat stencilAttachment);

		SAILOR_API void Apply(struct VkGraphicsPipelineCreateInfo& state) const override;

	private:
		TVector<VkFormat> m_colorAttachments;
		VkFormat m_depthAttachment;
		VkFormat m_stencilAttachment;
	};

	class VulkanStateViewport : public VulkanPipelineState
	{
	public:

		SAILOR_API VulkanStateViewport(float width, float height);
		SAILOR_API VulkanStateViewport(float x, float y, float width, float height, VkOffset2D scissorOffset, VkExtent2D scissorExtent, float minDepth = 0.0f, float maxDepth = 1.0f);

		SAILOR_API void Apply(struct VkGraphicsPipelineCreateInfo& state) const override;

		SAILOR_API const VkViewport& GetViewport() const { return m_viewport; }
		SAILOR_API const VkRect2D& GetScissor() const { return m_scissor; }

	private:

		VkViewport m_viewport;
		VkRect2D m_scissor;
		VkPipelineViewportStateCreateInfo m_viewportState;
	};

	class VulkanStateDynamicViewport : public VulkanPipelineState
	{
	public:
		SAILOR_API VulkanStateDynamicViewport();

		SAILOR_API void Apply(struct VkGraphicsPipelineCreateInfo& state) const override;
	private:

		VkPipelineViewportStateCreateInfo m_viewportState;
	};

	class VulkanStateVertexDescription : public VulkanPipelineState
	{
	public:

		SAILOR_API VulkanStateVertexDescription(const VkVertexInputBindingDescription& binding, const TVector<VkVertexInputAttributeDescription> attributes);

		SAILOR_API void Apply(struct VkGraphicsPipelineCreateInfo& state) const override;

	private:

		VkVertexInputBindingDescription m_bindingDescription;
		TVector<VkVertexInputAttributeDescription> m_attributeDescriptions;
		VkPipelineVertexInputStateCreateInfo m_vertexInput;
	};

	class VulkanStateInputAssembly : public VulkanPipelineState
	{
	public:

		SAILOR_API VulkanStateInputAssembly(VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, bool bPrimitiveRestartEnable = false);
		SAILOR_API void Apply(struct VkGraphicsPipelineCreateInfo& state) const override;

	private:

		VkPipelineInputAssemblyStateCreateInfo m_inputAssembly;
	};

	class VulkanStateRasterization : public VulkanPipelineState
	{
	public:

		SAILOR_API VulkanStateRasterization(bool bEnableDepthBias = false, float depthBias = 0.0f, VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT, VkPolygonMode fillMode = VK_POLYGON_MODE_FILL);
		SAILOR_API void Apply(struct VkGraphicsPipelineCreateInfo& state) const override;

	private:
		VkPipelineRasterizationStateCreateInfo m_rasterizer;
	};

	class VulkanStateMultisample : public VulkanPipelineState
	{
	public:

		SAILOR_API VulkanStateMultisample(VkSampleCountFlagBits samples);
		SAILOR_API void Apply(struct VkGraphicsPipelineCreateInfo& state) const override;

	private:
		VkPipelineMultisampleStateCreateInfo m_multisampling;
	};

	class VulkanStateColorBlending : public VulkanPipelineState
	{
	public:
		SAILOR_API VulkanStateColorBlending(bool blendEnable,
			VkBlendFactor            srcColorBlendFactor,
			VkBlendFactor            dstColorBlendFactor,
			VkBlendOp                colorBlendOp,
			VkBlendFactor            srcAlphaBlendFactor,
			VkBlendFactor            dstAlphaBlendFactor,
			VkBlendOp                alphaBlendOp,
			VkColorComponentFlags    colorWriteMask);

		SAILOR_API void Apply(struct VkGraphicsPipelineCreateInfo& state) const override;

	private:

		VkPipelineColorBlendAttachmentState m_colorBlendAttachment;
		VkPipelineColorBlendStateCreateInfo m_colorBlending;
	};

	class VulkanStateDynamicState : public VulkanPipelineState
	{
	public:
		SAILOR_API VulkanStateDynamicState();
		SAILOR_API void Apply(struct VkGraphicsPipelineCreateInfo& state) const override;

	private:

		VkPipelineDynamicStateCreateInfo m_dynamicState;
		TVector<VkDynamicState> m_dynamicStates;
	};

	class VulkanStateDepthStencil : public VulkanPipelineState
	{
	public:
		SAILOR_API VulkanStateDepthStencil(bool bEnableDepthTest = true, bool bEnableZWrite = true, VkCompareOp depthOp = VkCompareOp::VK_COMPARE_OP_GREATER);
		SAILOR_API void Apply(struct VkGraphicsPipelineCreateInfo& state) const override;

	private:

		VkPipelineDepthStencilStateCreateInfo m_depthStencil{};
	};

	struct VulkanPipelineStateBuilder
	{
		SAILOR_API VulkanPipelineStateBuilder(VulkanDevicePtr pDevice);
		SAILOR_API const TVector<VulkanPipelineStatePtr>& BuildPipeline(const RHI::RHIVertexDescriptionPtr& vertexDescription, RHI::EPrimitiveTopology topology, const RHI::RenderState& renderState);

	protected:

		VulkanStateColorBlendingPtr GetBlendState(RHI::EBlendMode mode);

		VulkanDevicePtr m_pDevice;
		TVector<VulkanStateColorBlendingPtr> m_blendModes;
		TConcurrentMap<RHI::RenderState, TVector<VulkanPipelineStatePtr>> m_cache;
	};
}