#pragma once
#include "VulkanApi.h"
#include "Memory/RefPtr.hpp"
#include "VulkanDevice.h"

namespace Sailor::GraphicsDriver::Vulkan
{
	class VulkanPipelineState;
	class VulkanShaderStage;
	class VulkanRenderPass;

	class VulkanPipelineLayout : public RHI::Resource, public RHI::IExplicitInitialization
	{
	public:

		VulkanPipelineLayout();
		VulkanPipelineLayout(VulkanDevicePtr pDevice,
			TVector<VulkanDescriptorSetLayoutPtr> descriptorsSet,
			TVector<VkPushConstantRange> pushConstantRanges,
			VkPipelineLayoutCreateFlags flags = 0);

		/// VkPipelineLayoutCreateInfo settings
		VkPipelineLayoutCreateFlags m_flags = 0;
		TVector<VulkanDescriptorSetLayoutPtr> m_descriptionSetLayouts;
		TVector<VkPushConstantRange> m_pushConstantRanges;

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
			TVector<VulkanShaderStagePtr> shaderStages,
			TVector<VulkanPipelineStatePtr> pipelineStates,
			uint32_t subpass = 0);

		/// VkGraphicsPipelineCreateInfo settings
		TVector<VulkanShaderStagePtr> m_stages;
		TVector<VulkanPipelineStatePtr> m_pipelineStates;
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
