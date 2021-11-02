#pragma once
#include "Core/RefPtr.hpp"
#include "GfxDevice/Vulkan/VulkanBuffer.h"
#include "Types.h"

using namespace GfxDevice::Vulkan;

namespace Sailor::RHI
{
	class Buffer : public Resource
	{
	public:
#if defined(VULKAN)
		struct
		{
			VulkanBufferPtr m_buffer;
		} m_vulkan;
#endif

		size_t GetSize() const;
	};
};
