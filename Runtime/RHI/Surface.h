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
		SAILOR_API RHISurface(RHITexturePtr target, RHITexturePtr resolved) :
			m_target(target),
			m_resolved(m_resolved)
		{}

		SAILOR_API RHITexturePtr GetTarget() { return m_target; }
		SAILOR_API RHITexturePtr GetResolved() { return m_resolved; }

	private:
		
		RHITexturePtr m_target;
		RHITexturePtr m_resolved;
	};
};
