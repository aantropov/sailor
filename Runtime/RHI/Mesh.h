#pragma once
#include "Memory/RefPtr.hpp"
#include "Renderer.h"
#include "Types.h"

namespace Sailor::RHI
{
	class Mesh : public Resource, public IDelayedInitialization
	{
	public:

		BufferPtr m_vertexBuffer;
		BufferPtr m_indexBuffer;
		VertexDescriptionPtr m_vertexDescription;

		SAILOR_API virtual bool IsReady() const override;

	protected:

	};
};
