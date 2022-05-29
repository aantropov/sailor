#pragma once
#include "Memory/RefPtr.hpp"
#include "Renderer.h"
#include "Types.h"
#include "Math/Math.h"
#include "Math/Bounds.h"

namespace Sailor::RHI
{
	class RHIMesh : public RHIResource, public IDelayedInitialization
	{
	public:

		RHIBufferPtr m_vertexBuffer{};
		RHIBufferPtr m_indexBuffer{};
		RHIVertexDescriptionPtr m_vertexDescription{};
		Math::AABB m_bounds{};

		SAILOR_API virtual bool IsReady() const override;

	protected:

	};
};
