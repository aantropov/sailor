#include "Containers/Vector.h"
#include "VulkanApi.h"
#include "VulkanPipeline.h"

#include "VulkanDescriptors.h"
#include "VulkanPipileneStates.h"
#include "VulkanShaderModule.h"
#include "VulkanRenderPass.h"

using namespace Sailor;
using namespace Sailor::GraphicsDriver::Vulkan;

VulkanPipelineLayout::VulkanPipelineLayout() :
	m_flags(0),
	m_pipelineLayout(nullptr)
{
}

VulkanPipelineLayout::VulkanPipelineLayout(
	VulkanDevicePtr pDevice,
	TVector<VulkanDescriptorSetLayoutPtr> descriptorsSet,
	TVector<RHI::ShaderLayoutBinding> shaderBindings,
	TVector<VkPushConstantRange> pushConstantRanges,
	VkPipelineLayoutCreateFlags flags) :
	m_flags(flags),
	m_descriptionSetLayouts(std::move(descriptorsSet)),
	m_pushConstantRanges(std::move(pushConstantRanges)),
	m_pDevice(std::move(pDevice)),
	m_pipelineLayout(nullptr),
	m_shaderBindings(std::move(shaderBindings))
{
}

VulkanPipelineLayout::~VulkanPipelineLayout()
{
	VulkanPipelineLayout::Release();
}

bool VulkanPipelineLayout::BuildPushConstantRanges(const TVector<VulkanShaderStagePtr>& stages,
	uint32_t maxSize, TVector<VkPushConstantRange>& outRanges)
{
	outRanges.Clear();
	VkPushConstantRange combined{ 0u, maxSize, 0u };
	uint32_t end = 0u;
	for (const auto& stage : stages)
	{
		for (const auto& range : stage->GetPushConstants())
		{
			if (range.stageFlags == 0u || range.size == 0u ||
				range.offset % 4u != 0u || range.size % 4u != 0u ||
				range.offset > maxSize || range.size > maxSize - range.offset)
			{
				return false;
			}
			combined.stageFlags |= range.stageFlags;
			combined.offset = std::min(combined.offset, range.offset);
			end = std::max(end, range.offset + range.size);
		}
	}
	if (combined.stageFlags != 0u)
	{
		// The RHI supplies one byte span shared by the declaring stages.
		combined.size = end - combined.offset;
		outRanges.Add(combined);
	}
	return true;
}

bool VulkanPipelineLayout::GetPushConstantUpdate(size_t offset, size_t size,
	const void*& data, VkPushConstantRange& outRange) const
{
	outRange = {};
	if (m_pushConstantRanges.IsEmpty())
	{
		return false;
	}
	const auto& range = m_pushConstantRanges[0];
	const size_t begin = std::max(offset, static_cast<size_t>(range.offset));
	const size_t end = static_cast<size_t>(range.offset) + range.size;
	if (begin >= end || begin - offset >= size)
	{
		return false;
	}
	const size_t updateSize = std::min(size - (begin - offset), end - begin);
	check(begin % 4u == 0u && updateSize % 4u == 0u);
	outRange = { range.stageFlags, static_cast<uint32_t>(begin), static_cast<uint32_t>(updateSize) };
	data = static_cast<const uint8_t*>(data) + (begin - offset);
	return true;
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

	for (size_t i = 0; i < m_descriptionSetLayouts.Num(); i++)
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

VulkanGraphicsPipeline::VulkanGraphicsPipeline(VulkanDevicePtr pDevice,
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

VulkanGraphicsPipeline::~VulkanGraphicsPipeline()
{
	Release();
}


void VulkanGraphicsPipeline::Release()
{
	if (m_pipeline)
	{
		vkDestroyPipeline(*m_pDevice, m_pipeline, nullptr);
		m_pipeline = 0;
	}
}

bool VulkanGraphicsPipeline::Compile()
{
	SAILOR_PROFILE_FUNCTION();

	if (m_pipeline)
	{
		return true;
	}

	m_layout->Compile();

	for (auto& shaderStage : m_stages)
	{
		shaderStage->Compile();
	}

	VkGraphicsPipelineCreateInfo pipelineInfo = {};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.layout = *m_layout;
	pipelineInfo.renderPass = m_renderPass ? *m_renderPass : nullptr;
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

	// Dynamic rendering can add MRT attachments to an existing material. Match
	// the blend array to this pipeline's actual target count, retaining the
	// material's alpha blend for every written output. A colour-only shader must
	// not overwrite another pass's velocity attachment with undefined values.
	TVector<VkPipelineColorBlendAttachmentState> blendAttachments;
	VkPipelineColorBlendStateCreateInfo colorBlending{};
	if (pipelineInfo.pColorBlendState && pipelineInfo.pNext)
	{
		const auto* rendering = static_cast<const VkPipelineRenderingCreateInfo*>(pipelineInfo.pNext);
		if (rendering->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO)
		{
			uint32_t outputMask = 0u;
			for (const auto& stage : m_stages)
				if (stage->m_stage == VK_SHADER_STAGE_FRAGMENT_BIT) outputMask = stage->GetFragmentOutputMask();
			colorBlending = *pipelineInfo.pColorBlendState;
			const auto attachment = colorBlending.attachmentCount ? colorBlending.pAttachments[0] : VkPipelineColorBlendAttachmentState{};
			blendAttachments.Resize(rendering->colorAttachmentCount);
			for (uint32_t i = 0u; i < rendering->colorAttachmentCount; ++i)
			{
				blendAttachments[i] = attachment;
				if (i > 0u && attachment.blendEnable)
				{
					// Auxiliary MRT coverage uses ordinary source-over, not the
					// HDR colour alpha channel's subtractive metadata operation.
					blendAttachments[i].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
					blendAttachments[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
					blendAttachments[i].colorBlendOp = VK_BLEND_OP_ADD;
					blendAttachments[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
					blendAttachments[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
					blendAttachments[i].alphaBlendOp = VK_BLEND_OP_ADD;
				}
				if (i >= 32u || (outputMask & (1u << i)) == 0u) blendAttachments[i].colorWriteMask = 0u;
			}
			colorBlending.attachmentCount = rendering->colorAttachmentCount;
			colorBlending.pAttachments = blendAttachments.GetData();
			pipelineInfo.pColorBlendState = &colorBlending;
		}
	}

	const VkResult result = vkCreateGraphicsPipelines(*m_pDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline);
	_freea(shaderStageCreateInfo);

	if (result != VK_SUCCESS || m_pipeline == VK_NULL_HANDLE)
	{
		m_pipeline = VK_NULL_HANDLE;
		SAILOR_LOG_ERROR("VulkanGraphicsPipeline::Compile: vkCreateGraphicsPipelines failed with VkResult %d.", static_cast<int32_t>(result));
		return false;
	}

	return true;
}

void VulkanGraphicsPipeline::ApplyStates(VkGraphicsPipelineCreateInfo& pipelineInfo) const
{
	for (auto pipelineState : m_pipelineStates)
	{
		pipelineState->Apply(pipelineInfo);
	}
}

// VulkanComputePipeline
VulkanComputePipeline::VulkanComputePipeline(VulkanDevicePtr pDevice,
	VulkanPipelineLayoutPtr pipelineLayout,
	VulkanShaderStagePtr shaderStage) :
	m_stage(std::move(shaderStage)),
	m_layout(std::move(pipelineLayout)),
	m_pDevice(std::move(pDevice))
{
}

VulkanComputePipeline::~VulkanComputePipeline()
{
	Release();
}

void VulkanComputePipeline::Release()
{
	if (m_pipeline)
	{
		vkDestroyPipeline(*m_pDevice, m_pipeline, nullptr);
		m_pipeline = 0;
	}
}

bool VulkanComputePipeline::Compile()
{
	SAILOR_PROFILE_FUNCTION();

	if (m_pipeline)
	{
		return true;
	}
	if (!m_layout || !m_stage)
	{
		SAILOR_LOG_ERROR("VulkanComputePipeline::Compile: pipeline layout or shader stage is unavailable.");
		return false;
	}

	m_layout->Compile();
	m_stage->Compile();

	// TODO: Check flags: VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT | VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT
	VkPipelineShaderStageCreateInfo shaderStageCreateInfo{};
	shaderStageCreateInfo.flags = 0;
	m_stage->Apply(shaderStageCreateInfo);

	// TODO: Check flags
	VkComputePipelineCreateInfo pipelineInfo = {};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.layout = *m_layout;
	pipelineInfo.flags = 0;
	pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
	pipelineInfo.pNext = nullptr;

	pipelineInfo.stage = shaderStageCreateInfo;

	const VkResult result = vkCreateComputePipelines(*m_pDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline);
	if (result != VK_SUCCESS || m_pipeline == VK_NULL_HANDLE)
	{
		m_pipeline = VK_NULL_HANDLE;
		SAILOR_LOG_ERROR("VulkanComputePipeline::Compile: vkCreateComputePipelines failed with VkResult %d.", static_cast<int32_t>(result));
		return false;
	}

	return true;
}
