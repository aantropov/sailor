#include "Fence.h"
#include "Types.h"
#include "GfxDevice/Vulkan/VulkanApi.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GfxDevice::Vulkan;

void Fence::Wait(uint64_t timeout) const
{
#if defined(VULKAN)
	VK_CHECK(m_vulkan.m_fence->Wait(timeout));
#endif
}

void Fence::Reset() const
{
#if defined(VULKAN)
	VK_CHECK(m_vulkan.m_fence->Reset());
#endif
}

bool Fence::IsFinished() const
{
#if defined(VULKAN)
	return m_vulkan.m_fence->Status() == VkResult::VK_SUCCESS;
#endif

	return true;
}