#pragma once
#include "VulkanApi.h"
#include "Core/RefPtr.hpp"
#include "VulkanDevice.h"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanPipelineState;
	class VulkanShaderStage;
	class VulkanRenderPass;

	class VulkanPipelineLayout : public RHI::Resource, public RHI::IExplicitInitialization
	{
	public:

		VulkanPipelineLayout();
		VulkanPipelineLayout(VulkanDevicePtr pDevice,
			std::vector<VulkanDescriptorSetLayoutPtr> descriptorsSet,
			std::vector<VkPushConstantRange> pushConstantRanges,
			VkPipelineLayoutCreateFlags flags = 0);

		/// VkPipelineLayoutCreateInfo settings
		VkPipelineLayoutCreateFlags m_flags = 0;
		std::vector<VulkanDescriptorSetLayoutPtr> m_descriptionSetLayouts;
		std::vector<VkPushConstantRange> m_pushConstantRanges;

		/// Vulkan VkPipelineLayout handle
		VkPipelineLayout* GetHandle() { return &m_pipelineLayout; }
		operator VkPipelineLayout() const { return m_pipelineLayout; }

		virtual void Compile() override;
		virtual void Release() override;

	protected:

		virtual ~VulkanPipelineLayout() override;
		VulkanDevicePtr m_pDevice;
		VkPipelineLayout m_pipelineLayout;
	};

	class VulkanPipeline : public RHI::Resource
	{
	public:

		VulkanPipeline() = default;

		VulkanPipeline(VulkanDevicePtr pDevice,
			VulkanPipelineLayoutPtr pipelineLayout,
			std::vector<VulkanShaderStagePtr> shaderStages,
			std::vector<VulkanPipelineStatePtr> pipelineStates,
			uint32_t subpass = 0);

		/// VkGraphicsPipelineCreateInfo settings
		std::vector<VulkanShaderStagePtr> m_stages;
		std::vector<VulkanPipelineStatePtr> m_pipelineStates;
		VulkanPipelineLayoutPtr m_layout;
		VulkanRenderPassPtr m_renderPass;
		uint32_t m_subpass;

		void Compile();
		void Release();

		operator VkPipeline() const { return m_pipeline; }

	protected:

		void ApplyStates(VkGraphicsPipelineCreateInfo& pipelineInfo) const;
		virtual ~VulkanPipeline();

		VkPipeline m_pipeline;
		VulkanDevicePtr m_pDevice;
	};
}
