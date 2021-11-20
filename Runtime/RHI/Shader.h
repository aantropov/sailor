#pragma once
#include "Core/RefPtr.hpp"
#include "Core/WeakPtr.hpp"
#include "Renderer.h"
#include "Types.h"
#include "GfxDevice/Vulkan/VulkanMemory.h"
#include "GfxDevice/Vulkan/VulkanBufferMemory.h"
#include "GfxDevice/Vulkan/VulkanBufferMemory.h"
#include "GfxDevice/Vulkan/VulkanApi.h"
#include "GfxDevice/Vulkan/VulkanDescriptors.h"

namespace Sailor::RHI
{
#if defined(VULKAN)
	using VulkanUniformBufferAllocator = TBlockAllocator<Sailor::Memory::GlobalVulkanBufferAllocator, VulkanBufferMemoryPtr>;
#endif

	class ShaderBinding : public Resource
	{
	public:
#if defined(VULKAN)
		struct
		{
			TWeakPtr<VulkanUniformBufferAllocator> m_uniformBufferAllocator;
			TMemoryPtr<Sailor::Memory::VulkanBufferMemoryPtr> m_valueBinding;
			VkDescriptorSetLayoutBinding m_descriptorSetLayout;
		} m_vulkan;
#endif
		SAILOR_API ~ShaderBinding() override;
		SAILOR_API bool IsBind() const;

		SAILOR_API const TexturePtr& GetTextureBinding() const { return m_textureBinding; }
		SAILOR_API const ShaderLayoutBinding& GetMembersInfo() const { return m_membersInfo; }

		SAILOR_API void SetTextureBinding(TexturePtr textureBinding) { m_textureBinding = textureBinding; }
		SAILOR_API void SetMembersInfo(const ShaderLayoutBinding& membersInfo) { m_membersInfo = membersInfo; }

		SAILOR_API bool FindVariableInUniformBuffer(const std::string& variable, ShaderLayoutBindingMember& outVariable) const;

	protected:

		TexturePtr m_textureBinding;
		ShaderLayoutBinding m_membersInfo;
	};

	class Shader : public Resource
	{
	public:

		SAILOR_API Shader(EShaderStage stage) : m_stage(stage) {}

#if defined(VULKAN)
		struct
		{
			Sailor::GfxDevice::Vulkan::VulkanShaderStagePtr m_shader;
		} m_vulkan;
#endif

		SAILOR_API EShaderStage GetStage() const { return m_stage; }

	protected:

		EShaderStage m_stage;
	};
};
