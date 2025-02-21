#include "VulkanShaderModule.h"
#include "RHI/Types.h"

#include <spirv_reflect.h>

using namespace Sailor;
using namespace Sailor::GraphicsDriver::Vulkan;

// from https://github.com/KhronosGroup/SPIRV-Reflect/blob/main/spirv_reflect.c
static int SortCompareUint32(const void* a, const void* b) {
	const uint32_t* p_a = (const uint32_t*)a;
	const uint32_t* p_b = (const uint32_t*)b;

	return (int)*p_a - (int)*p_b;
}

static SpvReflectResult EnumerateAllPushConstants(SpvReflectShaderModule* p_module, size_t* p_push_constant_count,
	uint32_t** p_push_constants) {
	*p_push_constant_count = p_module->push_constant_block_count;
	if (*p_push_constant_count == 0) {
		return SPV_REFLECT_RESULT_SUCCESS;
	}
	*p_push_constants = (uint32_t*)calloc(*p_push_constant_count, sizeof(**p_push_constants));

	if ((*p_push_constants) == nullptr) {
		return SPV_REFLECT_RESULT_ERROR_ALLOC_FAILED;
	}

	for (size_t i = 0; i < *p_push_constant_count; ++i) {
		(*p_push_constants)[i] = p_module->push_constant_blocks[i].spirv_id;
	}
	qsort(*p_push_constants, *p_push_constant_count, sizeof(**p_push_constants), SortCompareUint32);
	return SPV_REFLECT_RESULT_SUCCESS;
}
//

VulkanShaderStage::VulkanShaderStage(VkShaderStageFlagBits stage, const std::string& entryPointName, VulkanShaderModulePtr shaderModule) :
	m_stage(stage),
	m_module(shaderModule),
	m_entryPointName(entryPointName)
{
}

VulkanShaderStage::VulkanShaderStage(VkShaderStageFlagBits stage, const std::string& entryPointName, VulkanDevicePtr pDevice, const RHI::ShaderByteCode& spirv) :
	m_stage(stage),
	m_entryPointName(entryPointName)
{
	m_module = VulkanShaderModulePtr::Make(pDevice, spirv);
}

void VulkanShaderStage::Apply(VkPipelineShaderStageCreateInfo& stageInfo) const
{
	stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageInfo.stage = m_stage;
	stageInfo.module = *m_module;
	stageInfo.pName = m_entryPointName.c_str();
}

void VulkanShaderStage::Compile()
{
	if ((VkShaderModule)*m_module == nullptr)
	{
		m_module->Compile();
		ReflectDescriptorSetBindings(m_module->m_byteCode);
	}
}

void VulkanShaderStage::ReflectDescriptorSetBindings(const RHI::ShaderByteCode& code)
{
	SAILOR_PROFILE_FUNCTION();

	SpvReflectShaderModule module;
	SpvReflectResult result = spvReflectCreateShaderModule(code.Num() * sizeof(code[0]), &code[0], &module);

	check(result == SPV_REFLECT_RESULT_SUCCESS);
	check(module.entry_point_count > 0);

	if (result != SPV_REFLECT_RESULT_SUCCESS)
	{
		return;
	}

	const uint32_t numBoundVertexAttributes = module.entry_points[0].input_variable_count;
	for (uint32_t i = 0; i < numBoundVertexAttributes; i++)
	{
		m_vertexAttributeBindings.Insert(module.entry_points[0].input_variables[i]->location);
	}

	uint32_t count = 0;
	result = spvReflectEnumerateDescriptorSets(&module, &count, NULL);
	check(result == SPV_REFLECT_RESULT_SUCCESS);

	TVector<TMap<uint32_t, TPair<RHI::ShaderLayoutBinding, VkDescriptorSetLayoutBinding>>> bindings;
	bindings.Resize(count);

	TVector<SpvReflectDescriptorSet*> sets(count);
	result = spvReflectEnumerateDescriptorSets(&module, &count, sets.GetData());
	check(result == SPV_REFLECT_RESULT_SUCCESS);

	for (size_t i_set = 0; i_set < sets.Num(); ++i_set)
	{
		const SpvReflectDescriptorSet& reflSet = *(sets[i_set]);
		TMap<uint32_t, TPair<RHI::ShaderLayoutBinding, VkDescriptorSetLayoutBinding>>& binding = bindings[i_set];

		for (uint32_t i_binding = 0; i_binding < reflSet.binding_count; ++i_binding)
		{
			const SpvReflectDescriptorBinding& reflBinding = *(reflSet.bindings[i_binding]);

			RHI::ShaderLayoutBinding& rhiBinding = binding[reflBinding.binding].m_first = {};
			VkDescriptorSetLayoutBinding& layoutBinding = binding[reflBinding.binding].m_second = {};

			layoutBinding = VulkanApi::CreateDescriptorSetLayoutBinding(reflBinding.binding,
				static_cast<VkDescriptorType>(reflBinding.descriptor_type), reflBinding.count);

			/*for (uint32_t i_dim = 0; i_dim < reflBinding.array.dims_count; ++i_dim)
			{
				layoutBinding.descriptorCount *= reflBinding.array.dims[i_dim];
			}*/

			// TODO: Should we split Compute/Graphics bit?
			layoutBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;

			// Fill data
			rhiBinding.m_name = reflBinding.name ? std::string(reflBinding.name) : "";
			rhiBinding.m_type = (RHI::EShaderBindingType)reflBinding.descriptor_type;
			rhiBinding.m_binding = reflBinding.binding;
			rhiBinding.m_size = reflBinding.block.size;
			rhiBinding.m_set = reflBinding.set;
			rhiBinding.m_arrayCount = reflBinding.count;
			rhiBinding.m_paddedSize = reflBinding.block.padded_size;
			rhiBinding.m_textureType = (RHI::ETextureType)reflBinding.image.dim;

			uint32_t membersSize = 0;
			uint32_t maxMemberSize = 0;

			// We handle UBO as POD
			SpvReflectBlockVariable* blockContent = reflBinding.block.members;
			uint32_t blockCount = reflBinding.block.member_count;

			// We handle SSBO as instance's POD
			if (rhiBinding.m_type == RHI::EShaderBindingType::StorageBuffer)
			{
				blockContent = reflBinding.block.members[0].members;
				blockCount = reflBinding.block.members[0].member_count;
			}

			for (uint32_t i = 0; i < blockCount; i++)
			{
				RHI::ShaderLayoutBindingMember member;

				member.m_name = blockContent[i].name ? std::string(blockContent[i].name) : "";
				// Should we handle absolute offset in this way?
				member.m_absoluteOffset = membersSize;// +blockContent[i].absolute_offset;
				member.m_size = blockContent[i].size;
				member.m_type = (RHI::EShaderBindingMemberType)(blockContent[i].type_description->op);
				member.m_arrayDimensions = blockContent[i].array.dims_count;
				// TODO: implement multiple arrays
				member.m_arrayCount = blockContent[i].array.dims[0];
				member.m_arrayStride = blockContent[i].array.stride;

				membersSize += blockContent[i].padded_size;
				maxMemberSize = std::max(maxMemberSize, blockContent[i].padded_size);

				if (member.m_type == RHI::EShaderBindingMemberType::Array && blockContent[i].type_description)
				{
					const auto& typeFlags = blockContent[i].type_description->type_flags;
					if (typeFlags & SPV_REFLECT_TYPE_FLAG_FLOAT)
					{
						member.m_type = RHI::EShaderBindingMemberType::Float;
					}
					else if (typeFlags & SPV_REFLECT_TYPE_FLAG_BOOL)
					{
						member.m_type = RHI::EShaderBindingMemberType::Bool;
					}
					else if (typeFlags & SPV_REFLECT_TYPE_FLAG_INT)
					{
						member.m_type = RHI::EShaderBindingMemberType::Int;
					}

					if (typeFlags & SPV_REFLECT_TYPE_FLAG_MATRIX)
					{
						member.m_type = RHI::EShaderBindingMemberType::Matrix;
					}
					else if (typeFlags & SPV_REFLECT_TYPE_FLAG_VECTOR)
					{
						member.m_type = RHI::EShaderBindingMemberType::Vector;
					}
				}

				rhiBinding.m_members.Emplace(std::move(member));
			}

			if (rhiBinding.m_type == RHI::EShaderBindingType::StorageBuffer)
			{
				//We store 1 instance size in binding
				rhiBinding.m_size = membersSize;
				rhiBinding.m_paddedSize = reflBinding.block.members[0].padded_size;

				if (maxMemberSize > 0)
				{
					// std430 array allignment
					uint32_t allignment = 16 - reflBinding.block.members[0].padded_size % 16;
					rhiBinding.m_paddedSize += allignment != maxMemberSize ? allignment : 0;
				}
			}
		}
	}

	size_t pushConstantsCount = 0;
	uint32_t* pRawPushConstants = nullptr;
	result = EnumerateAllPushConstants(&module, &pushConstantsCount, &pRawPushConstants);
	check(result == SPV_REFLECT_RESULT_SUCCESS);

	m_pushConstants.Resize(pushConstantsCount);
	for (uint32_t i = 0; i < pushConstantsCount; i++)
	{
		m_pushConstants[i] = pRawPushConstants[i];
	}

	spvReflectDestroyShaderModule(&module);

	m_layoutBindings.Clear();
	m_layoutBindings.Resize(count);

	m_bindings.Clear();
	m_bindings.Resize(count);

	for (uint32_t i = 0; i < count; i++)
	{
		for (const auto& binding : bindings[i])
		{
			m_layoutBindings[i].Add(binding.m_second->m_second);
			m_bindings[i].Add(binding.m_second->m_first);
		}
	}
}

VulkanShaderModule::VulkanShaderModule(VulkanDevicePtr pDevice, const RHI::ShaderByteCode& spirv) : m_byteCode(spirv), m_pDevice(pDevice) {}

VulkanShaderModule::~VulkanShaderModule()
{
	VulkanShaderModule::Release();
}

void VulkanShaderModule::Compile()
{
	if (m_shaderModule)
	{
		return;
	}

	VkShaderModuleCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	createInfo.codeSize = m_byteCode.Num() * 4;
	createInfo.pCode = reinterpret_cast<const uint32_t*>(m_byteCode.GetData());

	VK_CHECK(vkCreateShaderModule(*m_pDevice, &createInfo, nullptr, &m_shaderModule));
}

void VulkanShaderModule::Release()
{
	vkDestroyShaderModule(*m_pDevice, m_shaderModule, nullptr);
	m_shaderModule = nullptr;
}