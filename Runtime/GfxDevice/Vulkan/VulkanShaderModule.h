#pragma once
#include "vulkan/vulkan_core.h"
#include "vulkan/vulkan.h"
#include "VulkanDevice.h"
#include "AssetRegistry/ShaderCompiler.h"
#include "Core/RefPtr.hpp"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanShaderModule;

	class VulkanShaderStage : public RHI::RHIResource, public RHI::IRHIStateModifier<VkPipelineShaderStageCreateInfo>
	{
	public:
		VulkanShaderStage() = default;
		VulkanShaderStage(VkShaderStageFlagBits stage, const std::string& entryPointName, TRefPtr<VulkanShaderModule> shaderModule);
		VulkanShaderStage(VkShaderStageFlagBits stage, const std::string& entryPointName, TRefPtr<VulkanDevice> pDevice, const ShaderCompiler::ByteCode& spirv);

		/// Vulkan VkPipelineShaderStageCreateInfo settings
		VkPipelineShaderStageCreateFlags m_flags = 0;
		VkShaderStageFlagBits m_stage = {};
		TRefPtr<VulkanShaderModule> m_module;
		std::string m_entryPointName;

		virtual void Apply(VkPipelineShaderStageCreateInfo& stageInfo) const override;
		void Compile();
	};

	class VulkanShaderModule : public RHI::RHIResource
	{
	public:

		VulkanShaderModule() = default;
		VulkanShaderModule(TRefPtr<VulkanDevice> pDevice, const ShaderCompiler::ByteCode& spirv);

		operator VkShaderModule() const { return m_shaderModule; }

		ShaderCompiler::ByteCode m_byteCode;

		void Compile();
		void Release();

	protected:

		virtual ~VulkanShaderModule() override;

		VkShaderModule m_shaderModule = nullptr;
		TRefPtr<VulkanDevice> m_pDevice;
	};
}
