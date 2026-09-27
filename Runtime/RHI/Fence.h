#pragma once
#include "Types.h"
#include "Memory/RefPtr.hpp"
#include "GraphicsDriver/Vulkan/VulkanFence.h"

using namespace GraphicsDriver::Vulkan;

namespace Sailor::RHI
{
	enum class EFenceStatus
	{
		Pending,
		Finished,
		Failed
	};

	class RHISemaphore : public RHIResource
	{
	public:

#if defined(SAILOR_BUILD_WITH_VULKAN)
		struct
		{
			VulkanSemaphorePtr m_semaphore;
		} m_vulkan;
#endif

	protected:
	};


	class RHIFence : public RHIResource, public IVisitor, public IDependent
	{
	public:
#if defined(SAILOR_BUILD_WITH_VULKAN)
		struct
		{
			VulkanFencePtr m_fence;
		} m_vulkan;
#endif

		SAILOR_API EFenceStatus Wait(uint64_t timeout = UINT64_MAX) const;
		SAILOR_API bool Reset() const;
		SAILOR_API EFenceStatus GetStatus() const;
		SAILOR_API bool IsFinished() const;
		bool HasFailed() const { return m_status.load(std::memory_order_acquire) == EFenceStatus::Failed; }
		SAILOR_API void MarkSubmissionFailed();

	protected:
#if defined(SAILOR_BUILD_WITH_VULKAN)
		EFenceStatus UpdateStatus(VkResult result) const;
#endif
		mutable std::atomic<EFenceStatus> m_status{ EFenceStatus::Pending };
	};
};
