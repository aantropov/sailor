#pragma once
#include "vulkan/vulkan_core.h"
#include "vulkan/vulkan.h"
#include "VulkanDevice.h"
#include "RHI/Types.h"
#include "Memory/RefPtr.hpp"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanShaderModule;

	class VulkanShaderStage : public RHI::Resource, public RHI::IExplicitInitialization, public RHI::IStateModifier<VkPipelineShaderStageCreateInfo>
	{
	public:
		VulkanShaderStage() = default;
		VulkanShaderStage(VkShaderStageFlagBits stage, const std::string& entryPointName, VulkanShaderModulePtr shaderModule);
		VulkanShaderStage(VkShaderStageFlagBits stage, const std::string& entryPointName, VulkanDevicePtr pDevice, const RHI::ShaderByteCode& spirv);

		/// Vulkan VkPipelineShaderStageCreateInfo settings
		VkPipelineShaderStageCreateFlags m_flags = 0;
		VkShaderStageFlagBits m_stage = {};
		VulkanShaderModulePtr m_module;
		std::string m_entryPointName;

		virtual void Apply(VkPipelineShaderStageCreateInfo& stageInfo) const override;
		virtual void Compile() override;
		virtual void Release() override {}

		const std::vector<std::vector<VkDescriptorSetLayoutBinding>>& GetDescriptorSetLayoutBindings() const { return m_layoutBindings; }
		const std::vector<std::vector<RHI::ShaderLayoutBinding>>& GetBindings() const { return m_bindings; }

	protected:

		void ReflectDescriptorSetBindings(const RHI::ShaderByteCode& code);

		std::vector<std::vector<VkDescriptorSetLayoutBinding>> m_layoutBindings;
		std::vector<std::vector<RHI::ShaderLayoutBinding>> m_bindings;
	};

	class VulkanShaderModule : public RHI::Resource, public RHI::IExplicitInitialization
	{
	public:

		VulkanShaderModule() = default;
		VulkanShaderModule(VulkanDevicePtr pDevice, const RHI::ShaderByteCode& spirv);

		operator VkShaderModule() const { return m_shaderModule; }

		RHI::ShaderByteCode m_byteCode;

		virtual void Compile() override;
		virtual void Release() override;

	protected:

		virtual ~VulkanShaderModule() override;

		VkShaderModule m_shaderModule = nullptr;
		VulkanDevicePtr m_pDevice;
	};
}
