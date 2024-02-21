#pragma once
#include "Memory/RefPtr.hpp"
#include "Types.h"
#include "Math/Math.h"
#include "Math/Bounds.h"

namespace Sailor::RHI
{
	class RHIMesh : public RHIResource, public IDelayedInitialization
	{
	public:

		SAILOR_API uint32_t GetIndexCount() const;
		SAILOR_API uint32_t GetFirstIndex() const;
		SAILOR_API uint32_t GetVertexOffset() const;

		RHIBufferPtr m_vertexBuffer{};
		RHIBufferPtr m_indexBuffer{};
		RHIVertexDescriptionPtr m_vertexDescription{};
		Math::AABB m_bounds{};

		SAILOR_API virtual bool IsReady() const override;

	protected:

	};
};
