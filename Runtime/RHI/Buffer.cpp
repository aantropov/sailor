#include "Buffer.h"
#include "Types.h"
#include "GfxDevice/Vulkan/VulkanBuffer.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GfxDevice::Vulkan;

size_t Buffer::GetSize() const
{
#if defined(VULKAN)
	return m_vulkan.m_buffer->m_size;
#endif
	return 0;
}