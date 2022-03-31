#pragma once
#include "Memory/RefPtr.hpp"
#include "Renderer.h"
#include "Types.h"

namespace Sailor::RHI
{
	class VertexDescription : public Resource
	{
	public:

		struct AttributeDescription
		{
			uint32_t m_location;
			uint32_t m_binding;
			EFormat m_format;
			uint32_t m_offset;
		};

		static constexpr uint32_t DefaultPositionLocation = 0;
		static constexpr uint32_t DefaultNormalLocation = 1;
		static constexpr uint32_t DefaultTexcoordLocation = 2;
		static constexpr uint32_t DefaultColorLocation = 3;

		VertexDescription(EPrimitiveTopology topology, size_t stride) : m_topology(topology), m_vertexStride(stride) {}

		SAILOR_API EPrimitiveTopology GetTopology() const { return m_topology; }
		SAILOR_API size_t GetVertexStride() const { return m_vertexStride; }

		SAILOR_API void AddAttribute(uint32_t location, uint32_t binding, EFormat format, uint32_t offset);

		SAILOR_API const TVector<AttributeDescription>& GetAttributeDescriptions() const { return m_attributes; }

	protected:

		TVector<AttributeDescription> m_attributes;
		size_t m_vertexStride = 0;
		EPrimitiveTopology m_topology = EPrimitiveTopology::TriangleList;
	};
};
