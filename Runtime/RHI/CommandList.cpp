#include "CommandList.h"
#include "Types.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

uint32_t RHICommandList::GetGPUCost() const
{
	uint32_t res = 0;

#if defined(SAILOR_BUILD_WITH_VULKAN)
	res = m_vulkan.m_commandBuffer->GetGPUCost();
#endif

	return res;
}
uint32_t RHICommandList::GetNumRecordedCommands() const
{
	uint32_t res = 0;

#if defined(SAILOR_BUILD_WITH_VULKAN)
	res = m_vulkan.m_commandBuffer->GetNumRecordedCommands();
#endif

	return res;
}