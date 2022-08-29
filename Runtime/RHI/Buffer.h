#pragma once
#include "Memory/RefPtr.hpp"
#include "Types.h"
#include "GraphicsDriver/Vulkan/VulkanBufferMemory.h"

namespace Sailor::RHI
{
	class RHIBuffer : public RHIResource
	{
	public:

		RHIBuffer(EBufferUsageFlags usage) : m_usage(usage) {}
		virtual ~RHIBuffer();

#if defined(SAILOR_BUILD_WITH_VULKAN)
		using VulkanBufferAllocator = TBlockAllocator<Sailor::Memory::GlobalVulkanBufferAllocator, VulkanBufferMemoryPtr>;

		struct
		{
			TWeakPtr<VulkanBufferAllocator> m_bufferAllocator;
			TMemoryPtr<Memory::VulkanBufferMemoryPtr> m_buffer;
		} m_vulkan;
#endif

		SAILOR_API EBufferUsageFlags GetUsage() const { return m_usage; }
		SAILOR_API uint32_t GetOffset() const;
		SAILOR_API size_t GetSize() const;

	protected:

		EBufferUsageFlags m_usage;
	};
};
