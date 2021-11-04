#pragma once
#include "Core/RefPtr.hpp"
#include "Renderer.h"
#include "Types.h"
#include "GfxDevice/Vulkan/VulkanApi.h"

namespace Sailor::RHI
{
	class Material : public Resource
	{
	public:
#if defined(VULKAN)
		struct
		{
			Sailor::GfxDevice::Vulkan::VulkanPipelinePtr m_pipeline;
		} m_vulkan;
#endif

	protected:

	};
};
