#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "GraphicsDriver/Vulkan/VulkanCommandBuffer.h"
#include "Types.h"

namespace Sailor::RHI
{
	class RHICommandList : public RHIResource
	{
	public:

		RHICommandList(bool bIsTransferOnly) : m_bIsTransferOnly(bIsTransferOnly) {}

#if defined(SAILOR_BUILD_WITH_VULKAN)
		struct
		{
			Sailor::GraphicsDriver::Vulkan::VulkanCommandBufferPtr m_commandBuffer;
		} m_vulkan;
#endif

		bool IsTransferOnly() const { return m_bIsTransferOnly; }

	protected:

		bool m_bIsTransferOnly = false;
	};
};
