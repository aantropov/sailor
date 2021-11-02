#pragma once
#include "Core/RefPtr.hpp"
#include "Renderer.h"
#include "Types.h"

namespace Sailor::RHI
{
	class Mesh : public Resource, public IObservable, public IDependent
	{
	public:

		BufferPtr m_vertexBuffer;
		BufferPtr m_indexBuffer;
		
		virtual void TraceVisit(class TRefPtr<Resource> visitor, bool& bShouldRemoveFromList) override;

		bool IsReady() const;
	protected:

	};
};
