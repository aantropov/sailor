#include "VulkanShaderModule.h"
#include "AssetRegistry/ShaderCompiler.h"

using namespace Sailor;
using namespace Sailor::GfxDevice::Vulkan;

VulkanShaderStage::VulkanShaderStage(VkShaderStageFlagBits stage, const std::string& entryPointName, TRefPtr<VulkanShaderModule> shaderModule) :
	m_stage(stage),
	m_module(shaderModule),
	m_entryPointName(entryPointName)
{
}

VulkanShaderStage::VulkanShaderStage(VkShaderStageFlagBits stage, const std::string& entryPointName, TRefPtr<VulkanDevice> pDevice, const ShaderCompiler::ByteCode& spirv) :
	m_stage(stage),
	m_entryPointName(entryPointName)
{
	m_module = TRefPtr<VulkanShaderModule>::Make(pDevice, spirv);
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
	m_module->Compile();
}

VulkanShaderModule::VulkanShaderModule(TRefPtr<VulkanDevice> pDevice, const ShaderCompiler::ByteCode& spirv) : m_pDevice(pDevice), m_byteCode(spirv) {}

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