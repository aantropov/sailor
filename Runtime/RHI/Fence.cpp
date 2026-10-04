#include "Fence.h"
#include "Types.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

EFenceStatus RHIFence::Wait(uint64_t timeout) const
{
	const auto status = m_status.load(std::memory_order_acquire);
	if (status != EFenceStatus::Pending) return status;
#if defined(SAILOR_BUILD_WITH_VULKAN)
	return m_vulkan.m_fence ? UpdateStatus(m_vulkan.m_fence->Wait(timeout)) : status;
#else
	return GetStatus();
#endif
}

bool RHIFence::Reset() const
{
	if (HasFailed()) return false;
#if defined(SAILOR_BUILD_WITH_VULKAN)
	if (!m_vulkan.m_fence) return false;
	const VkResult result = m_vulkan.m_fence->Reset();
	if (result == VK_ERROR_DEVICE_LOST) m_status.store(EFenceStatus::Failed, std::memory_order_release);
	if (result != VK_SUCCESS) return false;
#endif
	m_status.store(EFenceStatus::Pending, std::memory_order_release);
	return true;
}

EFenceStatus RHIFence::GetStatus() const
{
	const auto status = m_status.load(std::memory_order_acquire);
	if (status != EFenceStatus::Pending) return status;
#if defined(SAILOR_BUILD_WITH_VULKAN)
	return m_vulkan.m_fence ? UpdateStatus(m_vulkan.m_fence->Status()) : status;
#else
	m_status.store(EFenceStatus::Finished, std::memory_order_release);
	return EFenceStatus::Finished;
#endif
}

bool RHIFence::IsFinished() const
{
	return GetStatus() == EFenceStatus::Finished;
}

#if defined(SAILOR_BUILD_WITH_VULKAN)
EFenceStatus RHIFence::UpdateStatus(VkResult result) const
{
	// A query/wait error other than device loss is not proof of completion.
	if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
		return m_status.load(std::memory_order_acquire);

	const auto status = result == VK_SUCCESS ? EFenceStatus::Finished : EFenceStatus::Failed;
	auto expected = EFenceStatus::Pending;
	m_status.compare_exchange_strong(expected, status, std::memory_order_acq_rel, std::memory_order_acquire);
	return expected == EFenceStatus::Pending ? status : expected;
}
#endif

void RHIFence::MarkSubmissionFailed()
{
	// Submission failure is terminal; retries use a new fence.
	m_status.store(EFenceStatus::Failed, std::memory_order_release);
	TraceObservables();
	ClearDependencies();
	ClearObservables();
}
