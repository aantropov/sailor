#pragma once
#include "Core/RefPtr.hpp"
#include "Core/WeakPtr.hpp"
#include "Renderer.h"
#include "Types.h"
#include "GfxDevice/Vulkan/VulkanDevice.h"
#include "GfxDevice/Vulkan/VulkanMemory.h"
#include "GfxDevice/Vulkan/VulkanBufferMemory.h"
#include "GfxDevice/Vulkan/VulkanApi.h"
#include "GfxDevice/Vulkan/VulkanDescriptors.h"

namespace Sailor::RHI
{
	class ShaderBinding : public Resource
	{
	public:
#if defined(VULKAN)
		using VulkanBufferAllocator = TBlockAllocator<Sailor::Memory::GlobalVulkanBufferAllocator, VulkanBufferMemoryPtr>;

		struct
		{
			TWeakPtr<VulkanBufferAllocator> m_bufferAllocator;
			TMemoryPtr<Sailor::Memory::VulkanBufferMemoryPtr> m_valueBinding;
			VkDescriptorSetLayoutBinding m_descriptorSetLayout;
		} m_vulkan;
#endif
		SAILOR_API ~ShaderBinding() override;
		SAILOR_API bool IsBind() const;

		SAILOR_API const TexturePtr& GetTextureBinding() const { return m_textureBinding; }
		SAILOR_API const ShaderLayoutBinding& GetLayout() const { return m_bindingLayout; }

		SAILOR_API void SetTextureBinding(TexturePtr textureBinding) { m_textureBinding = textureBinding; }
		SAILOR_API void SetLayout(const ShaderLayoutBinding& layout) { m_bindingLayout = layout; }

		SAILOR_API bool FindVariableInUniformBuffer(const std::string& variable, ShaderLayoutBindingMember& outVariable) const;

	protected:

		TexturePtr m_textureBinding;
		ShaderLayoutBinding m_bindingLayout;
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
