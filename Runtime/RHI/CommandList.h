#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "GraphicsDriver/Vulkan/VulkanCommandBuffer.h"
#include "Types.h"

namespace Sailor::RHI
{
	class CommandList : public Resource
	{
	public:

#if defined(SAILOR_BUILD_WITH_VULKAN)
		struct
		{
			Sailor::GraphicsDriver::Vulkan::VulkanCommandBufferPtr m_commandBuffer;
		} m_vulkan;
#endif
	};
};
