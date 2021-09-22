#include <vector>
#include "VulkanApi.h"
#include "VulkanPipeline.h"

#include "VulkanPipileneStates.h"
#include "VulkanShaderModule.h"
#include "VulkanRenderPass.h"

using namespace Sailor;
using namespace Sailor::GfxDevice::Vulkan;

VulkanPipelineLayout::VulkanPipelineLayout() :
	m_flags(0)
{
}

VulkanPipelineLayout::VulkanPipelineLayout(
	TRefPtr<VulkanDevice> pDevice,
	const std::vector<VkDescriptorSetLayout>& descriptorsSet,
	const std::vector<VkPushConstantRange>& pushConstantRanges,
	VkPipelineLayoutCreateFlags flags) :
	m_pDevice(pDevice),
	m_flags(flags),
	m_descriptionSetLayouts(descriptorsSet),
	m_pushConstantRanges(pushConstantRanges)
{

}

VulkanPipelineLayout::~VulkanPipelineLayout()
{
	Release();
}

void VulkanPipelineLayout::Release()
{
	if (m_pipelineLayout)
	{
		vkDestroyPipelineLayout(*m_pDevice, m_pipelineLayout, nullptr);
	}
}

void VulkanPipelineLayout::Compile()
{
	if (m_pipelineLayout)
	{
		return;
	}

	//TODO Compile all description sets

	VkPipelineLayoutCreateInfo pipelineLayoutInfo;
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.flags = m_flags;
	pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(m_descriptionSetLayouts.size());
	pipelineLayoutInfo.pSetLayouts = m_descriptionSetLayouts.data();
	pipelineLayoutInfo.pushConstantRangeCount = static_cast<uint32_t>(m_pushConstantRanges.size());
	pipelineLayoutInfo.pPushConstantRanges = m_pushConstantRanges.data();
	pipelineLayoutInfo.pNext = nullptr;

	VK_CHECK(vkCreatePipelineLayout(*m_pDevice, &pipelineLayoutInfo, nullptr, &m_pipelineLayout));
}

VulkanPipeline::VulkanPipeline(TRefPtr<VulkanDevice> pDevice,
	TRefPtr<VulkanPipelineLayout> pipelineLayout,
	const std::vector<TRefPtr<VulkanShaderStage>>& shaderStages,
	const std::vector<TRefPtr<VulkanPipelineState>>& pipelineStates,
	uint32_t subpass) :	
	m_stages(shaderStages),
	m_pDevice(std::move(pDevice)),
	m_pipelineStates(pipelineStates),
	m_layout(std::move(pipelineLayout)),
	m_subpass(subpass)
{
}

VulkanPipeline::~VulkanPipeline()
{
	Release();
}


void VulkanPipeline::Release()
{
	vkDestroyPipeline(*m_pDevice, m_pipeline, nullptr);
}

void VulkanPipeline::Compile()
{
	m_layout->Compile();

	for (auto& shaderStage : m_stages)
	{
		shaderStage->Compile();
	}

	VkGraphicsPipelineCreateInfo pipelineInfo = {};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.layout = *m_layout;
	pipelineInfo.renderPass = *m_renderPass;
	pipelineInfo.subpass = m_subpass;
	pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
	pipelineInfo.pNext = nullptr;

	auto shaderStageCreateInfo = reinterpret_cast<VkPipelineShaderStageCreateInfo*>(_malloca(m_stages.size() * sizeof(VkPipelineShaderStageCreateInfo)));

	for (size_t i = 0; i < m_stages.size(); ++i)
	{
		shaderStageCreateInfo[i].flags = 0;
		shaderStageCreateInfo[i].pNext = nullptr;
		m_stages[i]->Apply(shaderStageCreateInfo[i]);
	}

	pipelineInfo.stageCount = static_cast<uint32_t>(m_stages.size());
	pipelineInfo.pStages = shaderStageCreateInfo;

	ApplyStates(pipelineInfo);
	
	VK_CHECK(vkCreateGraphicsPipelines(*m_pDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline));
	_freea(shaderStageCreateInfo);
}


void VulkanPipeline::ApplyStates(VkGraphicsPipelineCreateInfo& pipelineInfo) const
{
	for (auto pipelineState : m_pipelineStates)
	{
		pipelineState->Apply(pipelineInfo);
	}
}