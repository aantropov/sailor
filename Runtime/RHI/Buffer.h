#pragma once
#include "Core/RefPtr.hpp"
#include "Types.h"
#include "GfxDevice/Vulkan/VulkanApi.h"

namespace Sailor::RHI
{
	class Buffer : public Resource
	{
	public:
#if defined(VULKAN)
		struct
		{
			Sailor::GfxDevice::Vulkan::VulkanBufferPtr m_buffer;
		} m_vulkan;
#endif

		SAILOR_API size_t GetSize() const;
	};
};
