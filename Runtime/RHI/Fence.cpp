#include "Fence.h"
#include "Types.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

void RHIFence::Wait(uint64_t timeout) const
{
#if defined(SAILOR_BUILD_WITH_VULKAN)
	VK_CHECK(m_vulkan.m_fence->Wait(timeout));
#endif
}

void RHIFence::Reset() const
{
#if defined(SAILOR_BUILD_WITH_VULKAN)
	VK_CHECK(m_vulkan.m_fence->Reset());
#endif
}

bool RHIFence::IsFinished() const
{
#if defined(SAILOR_BUILD_WITH_VULKAN)
	return m_vulkan.m_fence && m_vulkan.m_fence->Status() == VkResult::VK_SUCCESS;
#endif

	return true;
}