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

		SAILOR_API void SetTopology(EPrimitiveTopology topology) { m_topology = topology; }
		SAILOR_API EPrimitiveTopology GetTopology() const { return m_topology; }		

	protected:
		
		EPrimitiveTopology m_topology = EPrimitiveTopology::TriangleList;
	};
};
