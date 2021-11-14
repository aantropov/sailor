#pragma once
#include "Core/RefPtr.hpp"
#include "Renderer.h"
#include "Types.h"
#include "GfxDevice/Vulkan/VulkanBufferMemory.h"
#include "GfxDevice/Vulkan/VulkanApi.h"

namespace Sailor::RHI
{
	class ShaderBinding : public Resource
	{
	public:
#if defined(VULKAN)
		struct
		{
			Sailor::GfxDevice::Vulkan::VulkanImageViewPtr m_textureBinding;
			Sailor::Memory::VulkanBufferMemoryPtr m_valueBinding;
		} m_vulkan;
#endif
	};

	class Shader : public Resource
	{
	public:

		Shader(EShaderStage stage) : m_stage(stage) {}

#if defined(VULKAN)
		struct
		{
			Sailor::GfxDevice::Vulkan::VulkanShaderStagePtr m_shader;
		} m_vulkan;
#endif

		EShaderStage GetStage() const { return m_stage; }

	protected:

		EShaderStage m_stage;
	};
};
