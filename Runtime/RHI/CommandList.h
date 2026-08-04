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

		RHICommandList(RHI::ECommandListQueue queue) : m_queue(queue) {}

#if defined(SAILOR_BUILD_WITH_VULKAN)
		struct
		{
			Sailor::GraphicsDriver::Vulkan::VulkanCommandBufferPtr m_commandBuffer;
		} m_vulkan;
#endif

		bool IsTransferOnly() const { return m_queue == RHI::ECommandListQueue::Transfer; }
		RHI::ECommandListQueue GetQueue() const { return m_queue; }
		SAILOR_API uint32_t GetGPUCost() const;
		SAILOR_API uint32_t GetNumRecordedCommands() const;
		SAILOR_API DrawCallStats GetRecordedDrawCallStats() const { return m_recordedDrawCallStats; }
		SAILOR_API void RecordDrawCallStats(uint32_t numInstances)
		{
			m_recordedDrawCallStats.m_numBatches++;
			m_recordedDrawCallStats.m_numInstances += numInstances;
		}

	protected:

		RHI::ECommandListQueue m_queue = RHI::ECommandListQueue::Graphics;
		DrawCallStats m_recordedDrawCallStats{};
	};
};
