#pragma once
#include "Defines.h"
#include "Core/RefPtr.hpp"
#include "GfxDevice/Vulkan/VulkanCommandBuffer.h"
#include "Types.h"

namespace Sailor::RHI
{
	class CommandList : public Resource
	{
	public:

#if defined(VULKAN)
		struct
		{
			TRefPtr<GfxDevice::Vulkan::VulkanCommandBuffer> m_commandBuffer;
		} m_vulkan;
#endif
	};
};
