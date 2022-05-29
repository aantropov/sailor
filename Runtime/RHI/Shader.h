#pragma once
#include "Memory/RefPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "Renderer.h"
#include "Types.h"
#include "GraphicsDriver/Vulkan/VulkanDevice.h"
#include "GraphicsDriver/Vulkan/VulkanMemory.h"
#include "GraphicsDriver/Vulkan/VulkanBufferMemory.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "GraphicsDriver/Vulkan/VulkanDescriptors.h"

namespace Sailor::RHI
{
	class RHIShaderBinding : public Resource
	{
	public:
#if defined(SAILOR_BUILD_WITH_VULKAN)
		using VulkanBufferAllocator = TBlockAllocator<Sailor::Memory::GlobalVulkanBufferAllocator, VulkanBufferMemoryPtr>;

		struct
		{
			TWeakPtr<VulkanBufferAllocator> m_bufferAllocator;
			TMemoryPtr<Sailor::Memory::VulkanBufferMemoryPtr> m_valueBinding;
			VkDescriptorSetLayoutBinding m_descriptorSetLayout{};
			uint32_t m_storageInstanceIndex = 0;
		} m_vulkan;
#endif

		SAILOR_API ~RHIShaderBinding() override;
		SAILOR_API bool IsBind() const;

		SAILOR_API const RHITexturePtr& GetTextureBinding() const { return m_textureBinding; }
		SAILOR_API const ShaderLayoutBinding& GetLayout() const { return m_bindingLayout; }
	
		SAILOR_API uint32_t GetStorageInstanceIndex()
		{
#if defined(SAILOR_BUILD_WITH_VULKAN)
			return m_vulkan.m_storageInstanceIndex;
#endif
			return 0;
		}

		SAILOR_API void SetTextureBinding(RHITexturePtr textureBinding) { m_textureBinding = textureBinding; }
		SAILOR_API void SetLayout(const ShaderLayoutBinding& layout) { m_bindingLayout = layout; }

		SAILOR_API bool FindVariableInUniformBuffer(const std::string& variable, ShaderLayoutBindingMember& outVariable) const;

	protected:

		RHITexturePtr m_textureBinding;
		ShaderLayoutBinding m_bindingLayout{};
	};

	class RHIShader : public Resource
	{
	public:

		SAILOR_API RHIShader(EShaderStage stage) : m_stage(stage) {}

#if defined(SAILOR_BUILD_WITH_VULKAN)
		struct
		{
			Sailor::GraphicsDriver::Vulkan::VulkanShaderStagePtr m_shader{};
		} m_vulkan;
#endif

		SAILOR_API EShaderStage GetStage() const { return m_stage; }

	protected:

		EShaderStage m_stage;
	};
};
