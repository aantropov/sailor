#pragma once
#include "Memory/RefPtr.hpp"
#include "Types.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"

namespace Sailor::RHI
{
	class Buffer : public Resource
	{
	public:

		Buffer(EBufferUsageFlags usage) : m_usage(usage) {}

#if defined(SAILOR_BUILD_WITH_VULKAN)
		struct
		{
			Sailor::GraphicsDriver::Vulkan::VulkanBufferPtr m_buffer;
		} m_vulkan;
#endif

		SAILOR_API EBufferUsageFlags GetUsage() const { return m_usage; }
		SAILOR_API size_t GetSize() const;

	protected:

		EBufferUsageFlags m_usage;

	};
};
