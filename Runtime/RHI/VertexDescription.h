#pragma once
#include "Memory/RefPtr.hpp"
#include "Types.h"

namespace Sailor::RHI
{
	class RHIVertexDescription : public RHIResource
	{
	public:

		struct AttributeDescription
		{
			uint32_t m_location;
			uint32_t m_binding;
			EFormat m_format;
			uint32_t m_offset;
		};

static constexpr uint32_t DefaultPositionBinding = 0;
static constexpr uint32_t DefaultNormalBinding = 1;
static constexpr uint32_t DefaultTexcoordBinding = 2;
static constexpr uint32_t DefaultColorBinding = 3;
static constexpr uint32_t DefaultTangentBinding = 4;
static constexpr uint32_t DefaultBitangentBinding = 5;
static constexpr uint32_t DefaultBoneIdsBinding = 6;
static constexpr uint32_t DefaultBoneWeightsBinding = 7;

		RHIVertexDescription() = default;

		SAILOR_API void SetVertexStride(size_t stride) { m_vertexStride = stride; }
		SAILOR_API size_t GetVertexStride() const { return m_vertexStride; }

		SAILOR_API void AddAttribute(uint32_t location, uint32_t binding, EFormat format, uint32_t offset);

		SAILOR_API const TVector<AttributeDescription>& GetAttributeDescriptions() const { return m_attributes; }
		SAILOR_API const VertexAttributeBits& GetVertexAttributeBits() const { return m_bits; }

	protected:

		TVector<AttributeDescription> m_attributes;
		VertexAttributeBits m_bits = 0u;
		size_t m_vertexStride = 0;
	};
};
