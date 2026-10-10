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
			TVector<RHI::ShaderLayoutBinding> shaderBindings,
			TVector<VkPushConstantRange> pushConstantRanges,
			VkPipelineLayoutCreateFlags flags = 0);

		/// VkPipelineLayoutCreateInfo settings
		VkPipelineLayoutCreateFlags m_flags = 0;
		TVector<VulkanDescriptorSetLayoutPtr> m_descriptionSetLayouts;
		// Empty or one byte span shared by the stages that declare push constants.
		TVector<VkPushConstantRange> m_pushConstantRanges;

		/// Vulkan VkPipelineLayout handle
		SAILOR_API VkPipelineLayout* GetHandle() { return &m_pipelineLayout; }
		SAILOR_API operator VkPipelineLayout() const { return m_pipelineLayout; }

		SAILOR_API const TVector<RHI::ShaderLayoutBinding>& GetShaderLayout() const { return m_shaderBindings; }
		SAILOR_API static bool BuildPushConstantRanges(const TVector<VulkanShaderStagePtr>& stages,
			uint32_t maxSize, TVector<VkPushConstantRange>& outRanges);
		SAILOR_API bool GetPushConstantUpdate(size_t offset, size_t size,
			const void*& data, VkPushConstantRange& outRange) const;

		SAILOR_API virtual void Compile() override;
		SAILOR_API virtual void Release() override;

	protected:

		SAILOR_API virtual ~VulkanPipelineLayout() override;
		VulkanDevicePtr m_pDevice;
		VkPipelineLayout m_pipelineLayout;
		TVector<RHI::ShaderLayoutBinding> m_shaderBindings;
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

		bool Compile();
		void Release();
		bool IsCompiled() const { return m_pipeline != VK_NULL_HANDLE; }
		VkSampleCountFlagBits GetMsaaSamples() const { return m_msaaSamples; }

		operator VkPipeline() const { return m_pipeline; }

	protected:

		void ApplyStates(VkGraphicsPipelineCreateInfo& pipelineInfo) const;
		virtual ~VulkanGraphicsPipeline();

		VkPipeline m_pipeline{};
		VkSampleCountFlagBits m_msaaSamples = VK_SAMPLE_COUNT_1_BIT;
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

		bool Compile();
		void Release();
		bool IsCompiled() const { return m_pipeline != VK_NULL_HANDLE; }

		operator VkPipeline() const { return m_pipeline; }

	protected:

		virtual ~VulkanComputePipeline();

		VkPipeline m_pipeline{};
		VulkanDevicePtr m_pDevice;
	};
}
