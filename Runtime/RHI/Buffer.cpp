#include "Buffer.h"
#include "Types.h"
#include "GraphicsDriver/Vulkan/VulkanBufferMemory.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

size_t RHIBuffer::GetSize() const
{
#if defined(SAILOR_BUILD_WITH_VULKAN)
	return m_vulkan.m_buffer.m_size;
#endif
	return 0;
}

uint32_t RHIBuffer::GetOffset() const
{
#if defined(SAILOR_BUILD_WITH_VULKAN)
	return (uint32_t)(*m_vulkan.m_buffer).m_offset;
#endif
	return 0;
}

RHIBuffer::~RHIBuffer()
{
#if defined(SAILOR_BUILD_WITH_VULKAN)
	if (m_vulkan.m_buffer)
	{
		if (m_vulkan.m_bufferAllocator)
		{
			auto allocator = m_vulkan.m_bufferAllocator.Lock();
			allocator->Free(m_vulkan.m_buffer);
		}
	}
#endif
}

size_t RHIBuffer::GetCompatibilityHashCode() const
{
#if defined(SAILOR_BUILD_WITH_VULKAN)
	if (m_vulkan.m_buffer)
	{
		return m_vulkan.m_buffer.m_ptr.m_buffer.GetHash();
	}
#endif

	return 0;
}