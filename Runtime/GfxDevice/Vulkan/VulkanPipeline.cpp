#include "Core/Vector.h"
#include "VulkanApi.h"
#include "VulkanPipeline.h"

#include "VulkanDescriptors.h"
#include "VulkanPipileneStates.h"
#include "VulkanShaderModule.h"
#include "VulkanRenderPass.h"

using namespace Sailor;
using namespace Sailor::GfxDevice::Vulkan;

VulkanPipelineLayout::VulkanPipelineLayout() :
	m_flags(0),
	m_pipelineLayout(nullptr)
{
}

VulkanPipelineLayout::VulkanPipelineLayout(
	VulkanDevicePtr pDevice,
	TVector<VulkanDescriptorSetLayoutPtr> descriptorsSet,
	TVector<VkPushConstantRange> pushConstantRanges,
	VkPipelineLayoutCreateFlags flags) :
	m_flags(flags),
	m_descriptionSetLayouts(std::move(descriptorsSet)),
	m_pushConstantRanges(std::move(pushConstantRanges)),
	m_pDevice(std::move(pDevice)),
	m_pipelineLayout(nullptr)
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

	const size_t stackArraySize = m_descriptionSetLayouts.Num() * sizeof(VkDescriptorSetLayout);
	auto ptr = reinterpret_cast<VkDescriptorSetLayout*>(_malloca(stackArraySize));
	memset(ptr, 0, stackArraySize);

	for(size_t i = 0; i < m_descriptionSetLayouts.Num(); i++)
	{
		m_descriptionSetLayouts[i]->Compile();
		ptr[i] = *m_descriptionSetLayouts[i];
	}
	
	//TODO Compile all description sets
	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.flags = m_flags;
	pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(m_descriptionSetLayouts.Num());
	pipelineLayoutInfo.pSetLayouts = ptr;
	pipelineLayoutInfo.pushConstantRangeCount = static_cast<uint32_t>(m_pushConstantRanges.Num());
	pipelineLayoutInfo.pPushConstantRanges = m_pushConstantRanges.GetData();
	pipelineLayoutInfo.pNext = nullptr;

	VK_CHECK(vkCreatePipelineLayout(*m_pDevice, &pipelineLayoutInfo, nullptr, &m_pipelineLayout));

	_freea(ptr);
}

VulkanPipeline::VulkanPipeline(VulkanDevicePtr pDevice,
	VulkanPipelineLayoutPtr pipelineLayout,
	TVector<VulkanShaderStagePtr> shaderStages,
	TVector<VulkanPipelineStatePtr> pipelineStates,
	uint32_t subpass) :
	m_stages(std::move(shaderStages)),
	m_pipelineStates(std::move(pipelineStates)),
	m_layout(std::move(pipelineLayout)),
	m_subpass(subpass),
	m_pDevice(std::move(pDevice))
{
}

VulkanPipeline::~VulkanPipeline()
{
	Release();
}


void VulkanPipeline::Release()
{
	if (m_pipeline)
	{
		vkDestroyPipeline(*m_pDevice, m_pipeline, nullptr);
		m_pipeline = 0;
	}
}

void VulkanPipeline::Compile()
{
	if(m_pipeline)
	{
		return;
	}

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

	const size_t stackArraySize = m_stages.Num() * sizeof(VkPipelineShaderStageCreateInfo);
	auto shaderStageCreateInfo = reinterpret_cast<VkPipelineShaderStageCreateInfo*>(_malloca(stackArraySize));
	memset(shaderStageCreateInfo, 0, stackArraySize);

	for (size_t i = 0; i < m_stages.Num(); ++i)
	{
		shaderStageCreateInfo[i].flags = 0;
		shaderStageCreateInfo[i].pNext = nullptr;
		m_stages[i]->Apply(shaderStageCreateInfo[i]);
	}

	pipelineInfo.stageCount = static_cast<uint32_t>(m_stages.Num());
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