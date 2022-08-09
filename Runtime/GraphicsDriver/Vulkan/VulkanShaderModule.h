#pragma once
#include "vulkan/vulkan_core.h"
#include "vulkan/vulkan.h"
#include "VulkanDevice.h"
#include "RHI/Types.h"
#include "Memory/RefPtr.hpp"

namespace Sailor::GraphicsDriver::Vulkan
{
	class VulkanShaderModule;

	class VulkanShaderStage : public RHI::RHIResource, public RHI::IExplicitInitialization, public RHI::IStateModifier<VkPipelineShaderStageCreateInfo>
	{
	public:
		SAILOR_API VulkanShaderStage() = default;
		SAILOR_API VulkanShaderStage(VkShaderStageFlagBits stage, const std::string& entryPointName, VulkanShaderModulePtr shaderModule);
		SAILOR_API VulkanShaderStage(VkShaderStageFlagBits stage, const std::string& entryPointName, VulkanDevicePtr pDevice, const RHI::ShaderByteCode& spirv);

		/// Vulkan VkPipelineShaderStageCreateInfo settings
		VkPipelineShaderStageCreateFlags m_flags = 0;
		VkShaderStageFlagBits m_stage = {};
		VulkanShaderModulePtr m_module;
		std::string m_entryPointName;

		SAILOR_API virtual void Apply(VkPipelineShaderStageCreateInfo& stageInfo) const override;
		SAILOR_API virtual void Compile() override;
		SAILOR_API virtual void Release() override {}

		SAILOR_API const TVector<TVector<VkDescriptorSetLayoutBinding>>& GetDescriptorSetLayoutBindings() const { return m_layoutBindings; }
		SAILOR_API const TVector<TVector<RHI::ShaderLayoutBinding>>& GetBindings() const { return m_bindings; }
		SAILOR_API const TVector<uint32_t>& GetPushConstants() const { return m_pushConstants; }
		
	protected:

		SAILOR_API void ReflectDescriptorSetBindings(const RHI::ShaderByteCode& code);

		TVector<TVector<VkDescriptorSetLayoutBinding>> m_layoutBindings;
		TVector<TVector<RHI::ShaderLayoutBinding>> m_bindings;
		TVector<uint32_t> m_pushConstants;
	};

	class VulkanShaderModule : public RHI::RHIResource, public RHI::IExplicitInitialization
	{
	public:

		SAILOR_API VulkanShaderModule() = default;
		SAILOR_API VulkanShaderModule(VulkanDevicePtr pDevice, const RHI::ShaderByteCode& spirv);

		SAILOR_API operator VkShaderModule() const { return m_shaderModule; }

		RHI::ShaderByteCode m_byteCode;

		SAILOR_API virtual void Compile() override;
		SAILOR_API virtual void Release() override;

	protected:

		SAILOR_API virtual ~VulkanShaderModule() override;

		VkShaderModule m_shaderModule = nullptr;
		VulkanDevicePtr m_pDevice;
	};
}
