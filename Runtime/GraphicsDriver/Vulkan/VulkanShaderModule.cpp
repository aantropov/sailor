#include "VulkanShaderModule.h"
#include "RHI/Types.h"

#include <spirv_reflect/spirv_reflect.h>
#include <spirv_reflect/spirv_reflect.c>

using namespace Sailor;
using namespace Sailor::GraphicsDriver::Vulkan;

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
	assert(result == SPV_REFLECT_RESULT_SUCCESS);

	uint32_t count = 0;
	result = spvReflectEnumerateDescriptorSets(&module, &count, NULL);
	assert(result == SPV_REFLECT_RESULT_SUCCESS);

	TVector<TMap<uint32_t, TPair<RHI::ShaderLayoutBinding, VkDescriptorSetLayoutBinding>>> bindings;
	bindings.Resize(count);

	TVector<SpvReflectDescriptorSet*> sets(count);
	result = spvReflectEnumerateDescriptorSets(&module, &count, sets.GetData());
	assert(result == SPV_REFLECT_RESULT_SUCCESS);

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
				static_cast<VkDescriptorType>(reflBinding.descriptor_type),
				1);

			for (uint32_t i_dim = 0; i_dim < reflBinding.array.dims_count; ++i_dim)
			{
				layoutBinding.descriptorCount *= reflBinding.array.dims[i_dim];
			}

			// We store different stages in one descriptor set
			layoutBinding.stageFlags = VK_SHADER_STAGE_ALL;
			// static_cast<VkShaderStageFlagBits>(module.shader_stage);

			// If we have type description then we have all info
			if (reflBinding.name)
			{
				rhiBinding.m_name = std::string(reflBinding.name);
				rhiBinding.m_type = (RHI::EShaderBindingType)reflBinding.descriptor_type;
				rhiBinding.m_binding = reflBinding.binding;
				rhiBinding.m_size = reflBinding.block.size;
				rhiBinding.m_set = reflBinding.set;
				rhiBinding.m_arrayCount = layoutBinding.descriptorCount;

				uint32_t membersSize = 0;
				for (uint32_t i = 0; i < reflBinding.block.member_count; i++)
				{
					RHI::ShaderLayoutBindingMember member;

					member.m_name = std::string(reflBinding.block.members[i].name);
					member.m_absoluteOffset = reflBinding.block.members[i].absolute_offset;
					member.m_size = reflBinding.block.members[i].size;
					member.m_type = (RHI::EShaderBindingMemberType)(reflBinding.block.members[i].type_description->op);
					member.m_arrayDimensions = reflBinding.block.members[i].array.dims_count;
					// TODO: implement multiple arrays
					member.m_arrayCount = reflBinding.block.members[i].array.dims[0];
					member.m_arrayStride = reflBinding.block.members[i].array.stride;
					
					membersSize += reflBinding.block.members[i].padded_size;

					if (member.m_type == RHI::EShaderBindingMemberType::Array && reflBinding.block.members[i].type_description)
					{
						const auto& typeFlags = reflBinding.block.members[i].type_description->type_flags;
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
				}
			}
		}
	}

	size_t pushConstantsCount = 0;
	uint32_t* pRawPushConstants = nullptr;
	result = EnumerateAllPushConstants(&module, &pushConstantsCount, &pRawPushConstants);
	assert(result == SPV_REFLECT_RESULT_SUCCESS);

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
		for (auto& binding : bindings[i])
		{
			m_layoutBindings[i].Add(std::move(binding.m_second.m_second));
			m_bindings[i].Add(std::move(binding.m_second.m_first));
		}
	}
}

VulkanShaderModule::VulkanShaderModule(VulkanDevicePtr pDevice, const RHI::ShaderByteCode& spirv) : m_pDevice(pDevice), m_byteCode(spirv) {}

VulkanShaderModule::~VulkanShaderModule()
{
	Release();
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