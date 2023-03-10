#pragma once
#include "Memory/RefPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "RHI/Types.h"
#include "Engine/Types.h"
#include "GraphicsDriver/Vulkan/VulkanDevice.h"
#include "GraphicsDriver/Vulkan/VulkanMemory.h"
#include "GraphicsDriver/Vulkan/VulkanBufferMemory.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "GraphicsDriver/Vulkan/VulkanDescriptors.h"

namespace Sailor::RHI
{
	class RHIShaderBinding : public RHIResource
	{
	public:
#if defined(SAILOR_BUILD_WITH_VULKAN)
		using VulkanBufferAllocator = TBlockAllocator<Sailor::Memory::GlobalVulkanBufferAllocator, VulkanBufferMemoryPtr>;

		struct
		{
			TManagedMemoryPtr<Sailor::Memory::VulkanBufferMemoryPtr, VulkanBufferAllocator> m_valueBinding;
			VkDescriptorSetLayoutBinding m_descriptorSetLayout{};
			uint32_t m_storageInstanceIndex = 0;
			bool m_bBindSsboWithOffset = false;
		} m_vulkan;
#endif

		SAILOR_API bool IsBind() const;

		SAILOR_API size_t GetCompatibilityHash() const;
		SAILOR_API RHITexturePtr GetTextureBinding(uint32_t index = 0) const { return m_textureBinding.Num() > index ? m_textureBinding[index] : nullptr; }
		SAILOR_API const TVector<RHITexturePtr>& GetTextureBindings() const { return m_textureBinding; }
		SAILOR_API const ShaderLayoutBinding& GetLayout() const { return m_bindingLayout; }

		SAILOR_API uint32_t GetStorageInstanceIndex() const
		{
#if defined(SAILOR_BUILD_WITH_VULKAN)
			return m_vulkan.m_storageInstanceIndex;
#endif
			return 0;
		}

		SAILOR_API size_t GetBufferOffset() const
		{
#if defined(SAILOR_BUILD_WITH_VULKAN)
			return (*m_vulkan.m_valueBinding->Get()).m_offset;
#endif
			return 0;
		}

		SAILOR_API void SetTextureBinding(const TVector<RHITexturePtr>& textureBinding) { m_textureBinding = textureBinding; }
		SAILOR_API void SetLayout(const ShaderLayoutBinding& layout) { m_bindingLayout = layout; }

		SAILOR_API bool FindVariableInUniformBuffer(const std::string& variable, ShaderLayoutBindingMember& outVariable) const;

	protected:

		TVector<RHITexturePtr> m_textureBinding{};
		ShaderLayoutBinding m_bindingLayout{};
	};

	class RHIShader : public RHIResource
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
