#pragma once
#include "VulkanApi.h"
#include "Core/RefPtr.hpp"
#include "VulkanDevice.h"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanPipelineState;
	class VulkanShaderStage;
	class VulkanRenderPass;

	class VulkanPipelineLayout : public RHI::RHIResource
	{
	public:

		VulkanPipelineLayout();
		VulkanPipelineLayout(TRefPtr<VulkanDevice> pDevice,
			std::vector<VkDescriptorSetLayout> descriptorsSet,
			std::vector<VkPushConstantRange> pushConstantRanges,
			VkPipelineLayoutCreateFlags flags = 0);

		/// VkPipelineLayoutCreateInfo settings
		VkPipelineLayoutCreateFlags m_flags = 0;
		std::vector<VkDescriptorSetLayout> m_descriptionSetLayouts;
		std::vector<VkPushConstantRange> m_pushConstantRanges;

		/// Vulkan VkPipelineLayout handle
		VkPipelineLayout* GetHandle() { return &m_pipelineLayout; }
		operator VkPipelineLayout() const { return m_pipelineLayout; }

		void Compile();
		void Release();

	protected:

		virtual ~VulkanPipelineLayout() override;
		TRefPtr<VulkanDevice> m_pDevice;
		VkPipelineLayout m_pipelineLayout;
	};

	class VulkanPipeline : public RHI::RHIResource
	{
	public:

		VulkanPipeline() = default;

		VulkanPipeline(TRefPtr<VulkanDevice> pDevice,
			TRefPtr<VulkanPipelineLayout> pipelineLayout,
			std::vector<TRefPtr<VulkanShaderStage>> shaderStages,
			std::vector<TRefPtr<VulkanPipelineState>> pipelineStates,
			uint32_t subpass = 0);

		/// VkGraphicsPipelineCreateInfo settings
		std::vector<TRefPtr<VulkanShaderStage>> m_stages;
		std::vector<TRefPtr<VulkanPipelineState>> m_pipelineStates;
		TRefPtr<VulkanPipelineLayout> m_layout;
		TRefPtr<VulkanRenderPass> m_renderPass;
		uint32_t m_subpass;

		void Compile();
		void Release();

		operator VkPipeline() const { return m_pipeline; }

	protected:

		void ApplyStates(VkGraphicsPipelineCreateInfo& pipelineInfo) const;
		virtual ~VulkanPipeline();

		VkPipeline m_pipeline;
		TRefPtr<VulkanDevice> m_pDevice;
	};
}
