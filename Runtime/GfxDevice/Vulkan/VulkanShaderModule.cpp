#include "VulkanShaderModule.h"
#include "AssetRegistry/ShaderCompiler.h"

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

VulkanShaderStage::VulkanShaderStage(VkShaderStageFlagBits stage, const std::string& entryPointName, VulkanDevicePtr pDevice, const ShaderCompiler::ByteCode& spirv) :
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

void VulkanShaderStage::ReflectDescriptorSetBindings(const ShaderCompiler::ByteCode& code)
{
	SAILOR_PROFILE_FUNCTION();

	SpvReflectShaderModule module;

	SpvReflectResult result = spvReflectCreateShaderModule(code.size() * sizeof(code[0]), &code[0], &module);
	assert(result == SPV_REFLECT_RESULT_SUCCESS);

	uint32_t count = 0;
	result = spvReflectEnumerateDescriptorSets(&module, &count, NULL);
	assert(result == SPV_REFLECT_RESULT_SUCCESS);

	std::vector<std::unordered_map<uint32_t, VkDescriptorSetLayoutBinding>> bindings;
	bindings.resize(count);

	std::vector<SpvReflectDescriptorSet*> sets(count);
	result = spvReflectEnumerateDescriptorSets(&module, &count, sets.data());
	assert(result == SPV_REFLECT_RESULT_SUCCESS);

	for (size_t i_set = 0; i_set < sets.size(); ++i_set)
	{
		const SpvReflectDescriptorSet& reflSet = *(sets[i_set]);
		std::unordered_map<uint32_t, VkDescriptorSetLayoutBinding>& binding = bindings[i_set];

		for (uint32_t i_binding = 0; i_binding < reflSet.binding_count; ++i_binding)
		{
			const SpvReflectDescriptorBinding& reflBinding = *(reflSet.bindings[i_binding]);

			VkDescriptorSetLayoutBinding& layoutBinding = binding[reflBinding.binding] = {};
			layoutBinding.binding = reflBinding.binding;
			layoutBinding.descriptorType = static_cast<VkDescriptorType>(reflBinding.descriptor_type);
			layoutBinding.descriptorCount = 1;

			for (uint32_t i_dim = 0; i_dim < reflBinding.array.dims_count; ++i_dim)
			{
				layoutBinding.descriptorCount *= reflBinding.array.dims[i_dim];
			}

			layoutBinding.stageFlags = static_cast<VkShaderStageFlagBits>(module.shader_stage);
		}
	}

	spvReflectDestroyShaderModule(&module);

	m_layoutBindings.clear();
	m_layoutBindings.resize(count);

	for (uint32_t i = 0; i < count; i++)
	{
		for (auto& binding : bindings[i])
		{
			m_layoutBindings[i].push_back(std::move(binding.second));
		}
	}
}


VulkanShaderModule::VulkanShaderModule(VulkanDevicePtr pDevice, const ShaderCompiler::ByteCode& spirv) : m_pDevice(pDevice), m_byteCode(spirv) {}

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