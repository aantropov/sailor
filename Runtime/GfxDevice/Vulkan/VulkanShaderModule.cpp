#include "VulkanShaderModule.h"
#include "RHI/Types.h"

#include <spirv_reflect/spirv_reflect.h>
#include <spirv_reflect/spirv_reflect.c>

using namespace Sailor;
using namespace Sailor::GfxDevice::Vulkan;

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

	SpvReflectResult result = spvReflectCreateShaderModule(code.size() * sizeof(code[0]), &code[0], &module);
	assert(result == SPV_REFLECT_RESULT_SUCCESS);

	uint32_t count = 0;
	result = spvReflectEnumerateDescriptorSets(&module, &count, NULL);
	assert(result == SPV_REFLECT_RESULT_SUCCESS);

	std::vector<std::unordered_map<uint32_t, std::pair<RHI::ShaderLayoutBinding, VkDescriptorSetLayoutBinding>>> bindings;
	bindings.resize(count);

	std::vector<SpvReflectDescriptorSet*> sets(count);
	result = spvReflectEnumerateDescriptorSets(&module, &count, sets.data());
	assert(result == SPV_REFLECT_RESULT_SUCCESS);

	for (size_t i_set = 0; i_set < sets.size(); ++i_set)
	{
		const SpvReflectDescriptorSet& reflSet = *(sets[i_set]);
		std::unordered_map<uint32_t, std::pair<RHI::ShaderLayoutBinding, VkDescriptorSetLayoutBinding>>& binding = bindings[i_set];

		for (uint32_t i_binding = 0; i_binding < reflSet.binding_count; ++i_binding)
		{
			const SpvReflectDescriptorBinding& reflBinding = *(reflSet.bindings[i_binding]);

			RHI::ShaderLayoutBinding& rhiBinding = binding[reflBinding.binding].first = {};
			VkDescriptorSetLayoutBinding& layoutBinding = binding[reflBinding.binding].second = {};
			layoutBinding.binding = reflBinding.binding;
			layoutBinding.descriptorType = static_cast<VkDescriptorType>(reflBinding.descriptor_type);
			layoutBinding.descriptorCount = 1;

			for (uint32_t i_dim = 0; i_dim < reflBinding.array.dims_count; ++i_dim)
			{
				layoutBinding.descriptorCount *= reflBinding.array.dims[i_dim];
			}

			layoutBinding.stageFlags = static_cast<VkShaderStageFlagBits>(module.shader_stage);

			// If we have type description then we have all info
			if (reflBinding.name)
			{
				rhiBinding.m_name = std::string(reflBinding.name);
				rhiBinding.m_type = (RHI::EShaderBindingType)reflBinding.descriptor_type;
				
				for (uint32_t i = 0; i < reflBinding.block.member_count; i++)
				{
					RHI::ShaderLayoutBindingMember member;

					member.m_name = std::string(reflBinding.block.members[i].name);
					member.m_offset = reflBinding.block.members[i].absolute_offset;
					member.m_size = reflBinding.block.members[i].size;
					member.m_type = (RHI::EShaderBindingMemberType)(reflBinding.block.members[i].type_description->op);

					rhiBinding.m_members.emplace_back(std::move(member));
				}
			}
		}
	}

	spvReflectDestroyShaderModule(&module);

	m_layoutBindings.clear();
	m_layoutBindings.resize(count);

	m_bindings.clear();
	m_bindings.resize(count);

	for (uint32_t i = 0; i < count; i++)
	{
		for (auto& binding : bindings[i])
		{
			m_layoutBindings[i].push_back(std::move(binding.second.second));
			m_bindings[i].push_back(std::move(binding.second.first));
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
	createInfo.codeSize = m_byteCode.size() * 4;
	createInfo.pCode = reinterpret_cast<const uint32_t*>(m_byteCode.data());

	VK_CHECK(vkCreateShaderModule(*m_pDevice, &createInfo, nullptr, &m_shaderModule));
}

void VulkanShaderModule::Release()
{
	vkDestroyShaderModule(*m_pDevice, m_shaderModule, nullptr);
	m_shaderModule = nullptr;
}