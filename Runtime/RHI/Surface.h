#pragma once
#include "Memory/RefPtr.hpp"
#include "GraphicsDriver/Vulkan/VulkanBuffer.h"
#include "Types.h"

using namespace GraphicsDriver::Vulkan;

namespace Sailor::RHI
{
	class RHISurface : public RHIResource, public IDelayedInitialization
	{
	public:
		SAILOR_API RHISurface(RHIRenderTargetPtr target, RHIRenderTargetPtr resolved, bool needsResolve) :
			m_target(target),
			m_resolved(resolved),
			m_bNeedsResolve(needsResolve)
		{}

		SAILOR_API RHIRenderTargetPtr GetTarget() const { return m_target; }
		SAILOR_API RHIRenderTargetPtr GetResolved() const { return m_resolved; }
		SAILOR_API bool NeedsResolve() const { return m_bNeedsResolve; }

	private:

		RHIRenderTargetPtr m_target{};
		RHIRenderTargetPtr m_resolved{};
		bool m_bNeedsResolve = false;

		friend class IGraphicsDriver;
	};
};
