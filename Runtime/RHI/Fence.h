#pragma once
#include "Renderer.h"
#include "Memory/RefPtr.hpp"
#include "GraphicsDriver/Vulkan/VulkanFence.h"
#include "Types.h"

using namespace GraphicsDriver::Vulkan;

namespace Sailor::RHI
{
	class Semaphore : public Resource
	{
	public:

#if defined(VULKAN)
		struct
		{
			VulkanSemaphorePtr m_semaphore;
		} m_vulkan;
#endif

	protected:
	};


	class Fence : public Resource, public IVisitor, public IDependent
	{
	public:
#if defined(VULKAN)
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
