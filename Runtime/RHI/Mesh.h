#pragma once
#include "Memory/RefPtr.hpp"
#include "Renderer.h"
#include "Types.h"
#include "Math/Math.h"

namespace Sailor::RHI
{
	class Mesh : public Resource, public IDelayedInitialization
	{
	public:

		BufferPtr m_vertexBuffer{};
		BufferPtr m_indexBuffer{};
		VertexDescriptionPtr m_vertexDescription{};
		Math::AABB m_bounds{};

		SAILOR_API virtual bool IsReady() const override;

	protected:

	};
};
