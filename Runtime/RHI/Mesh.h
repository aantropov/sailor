#pragma once
#include "Core/RefPtr.hpp"
#include "Renderer.h"
#include "Types.h"

namespace Sailor::RHI
{
	class Mesh : public Resource, public IDelayedInitialization
	{
	public:

		BufferPtr m_vertexBuffer;
		BufferPtr m_indexBuffer;

		virtual SAILOR_API bool IsReady() const override;

	protected:

	};
};
