#pragma once
#include "VulkanApi.h"
#include "Core/RefPtr.hpp"
#include "VulkanDevice.h"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanShaderStage;
	class VulkanRenderPass;

	class VulkanPipelineLayout : public TRefBase
	{
	public:

		VulkanPipelineLayout();
		VulkanPipelineLayout(TRefPtr<VulkanDevice> pDevice, const std::vector<VkDescriptorSetLayout>& descriptorsSet, const std::vector<VkPushConstantRange>& pushConstantRanges, VkPipelineLayoutCreateFlags flags = 0);

		/// VkPipelineLayoutCreateInfo settings
		VkPipelineLayoutCreateFlags m_flags = 0;
		std::vector<VkDescriptorSetLayout> m_descriptionSetLayouts;
		std::vector<VkPushConstantRange> m_pushConstantRanges;

		/// Vulkan VkPipelineLayout handle
		operator VkPipelineLayout() const { return m_pipelineLayout; }

		void Compile();
		void Release();

	protected:

		virtual ~VulkanPipelineLayout();
		TRefPtr<VulkanDevice> m_pDevice;
		VkPipelineLayout m_pipelineLayout;
	};

	class VulkanGraphicsPipeline : public TRefBase
	{
	public:

		VulkanGraphicsPipeline() = default;

		VulkanGraphicsPipeline(TRefPtr<VulkanDevice> pDevice,
			TRefPtr<VulkanPipelineLayout> pipelineLayout,
			const std::vector<TRefPtr<VulkanShaderStage>>& shaderStages,
			//const GraphicsPipelineState& pipelineStates, 
			uint32_t subpass = 0);

		/// VkGraphicsPipelineCreateInfo settings
		std::vector<TRefPtr<VulkanShaderStage>> m_stages;
		//std::vector<GraphicsPipelineState> pipelineStates;
		TRefPtr<VulkanPipelineLayout> m_layout;
		TRefPtr<VulkanRenderPass> m_renderPass;
		uint32_t m_subpass;

		void Compile();
		void Release();

		operator VkPipeline() const { return m_pipeline; }

	protected:

		virtual ~VulkanGraphicsPipeline();

		VkPipeline m_pipeline;
		TRefPtr<VulkanDevice> m_pDevice;
	};
}
