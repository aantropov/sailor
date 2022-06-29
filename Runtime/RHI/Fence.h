#pragma once
#include "Types.h"
#include "Memory/RefPtr.hpp"
#include "GraphicsDriver/Vulkan/VulkanFence.h"

using namespace GraphicsDriver::Vulkan;

namespace Sailor::RHI
{
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

		SAILOR_API void Wait(uint64_t timeout = UINT64_MAX) const;
		SAILOR_API void Reset() const;
		SAILOR_API bool IsFinished() const;

	protected:
	};
};
