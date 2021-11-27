#pragma once
#include "Memory/RefPtr.hpp"
#include "Types.h"
#include "GfxDevice/Vulkan/VulkanApi.h"

namespace Sailor::RHI
{
	class Buffer : public Resource
	{
	public:

		Buffer(EBufferUsageFlags usage) : m_usage(usage) {}

#if defined(VULKAN)
		struct
		{
			Sailor::GfxDevice::Vulkan::VulkanBufferPtr m_buffer;
		} m_vulkan;
#endif

		SAILOR_API EBufferUsageFlags GetUsage() const { return m_usage; }
		SAILOR_API size_t GetSize() const;

	protected:

		EBufferUsageFlags m_usage;

	};
};
