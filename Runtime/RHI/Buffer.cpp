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