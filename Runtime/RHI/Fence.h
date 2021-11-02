#pragma once
#include "Renderer.h"
#include "Core/RefPtr.hpp"
#include "GfxDevice/Vulkan/VulkanFence.h"
#include "Types.h"

using namespace GfxDevice::Vulkan;

namespace Sailor::RHI
{
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
