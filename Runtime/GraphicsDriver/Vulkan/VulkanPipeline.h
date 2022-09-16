#pragma once
#include "VulkanApi.h"
#include "Memory/RefPtr.hpp"
#include "VulkanDevice.h"

namespace Sailor::GraphicsDriver::Vulkan
{
	class VulkanPipelineState;
	class VulkanShaderStage;
	class VulkanRenderPass;

	class VulkanPipelineLayout : public RHI::RHIResource, public RHI::IExplicitInitialization
	{
	public:

		SAILOR_API VulkanPipelineLayout();
		SAILOR_API VulkanPipelineLayout(VulkanDevicePtr pDevice,
			TVector<VulkanDescriptorSetLayoutPtr> descriptorsSet,
			TVector<VkPushConstantRange> pushConstantRanges,
			VkPipelineLayoutCreateFlags flags = 0);

		/// VkPipelineLayoutCreateInfo settings
		VkPipelineLayoutCreateFlags m_flags = 0;
		TVector<VulkanDescriptorSetLayoutPtr> m_descriptionSetLayouts;
		TVector<VkPushConstantRange> m_pushConstantRanges;

		/// Vulkan VkPipelineLayout handle
		SAILOR_API VkPipelineLayout* GetHandle() { return &m_pipelineLayout; }
		SAILOR_API operator VkPipelineLayout() const { return m_pipelineLayout; }

		SAILOR_API virtual void Compile() override;
		SAILOR_API virtual void Release() override;

	protected:

		SAILOR_API virtual ~VulkanPipelineLayout() override;
		VulkanDevicePtr m_pDevice;
		VkPipelineLayout m_pipelineLayout;
	};

	class VulkanGraphicsPipeline : public RHI::RHIResource
	{
	public:

		VulkanGraphicsPipeline() = default;

		VulkanGraphicsPipeline(VulkanDevicePtr pDevice,
			VulkanPipelineLayoutPtr pipelineLayout,
			TVector<VulkanShaderStagePtr> shaderStages,
			TVector<VulkanPipelineStatePtr> pipelineStates,
			uint32_t subpass = 0);

		/// VkGraphicsPipelineCreateInfo settings
		TVector<VulkanShaderStagePtr> m_stages;
		TVector<VulkanPipelineStatePtr> m_pipelineStates;
		VulkanPipelineLayoutPtr m_layout;

		// Actually not used and disabled by VulkanStateDynamicRendering
		VulkanRenderPassPtr m_renderPass;
		uint32_t m_subpass;

		void Compile();
		void Release();

		operator VkPipeline() const { return m_pipeline; }

	protected:

		void ApplyStates(VkGraphicsPipelineCreateInfo& pipelineInfo) const;
		virtual ~VulkanGraphicsPipeline();

		VkPipeline m_pipeline{};
		VulkanDevicePtr m_pDevice;
	};

	class VulkanComputePipeline : public RHI::RHIResource
	{
	public:

		VulkanComputePipeline() = default;

		VulkanComputePipeline(VulkanDevicePtr pDevice,
			VulkanPipelineLayoutPtr pipelineLayout,
			VulkanShaderStagePtr computeShaderStage);

		/// VkComputePipelineCreateInfo settings
		VulkanShaderStagePtr m_stage;
		VulkanPipelineLayoutPtr m_layout;

		void Compile();
		void Release();

		operator VkPipeline() const { return m_pipeline; }

	protected:

		virtual ~VulkanComputePipeline();

		VkPipeline m_pipeline{};
		VulkanDevicePtr m_pDevice;
	};
}
