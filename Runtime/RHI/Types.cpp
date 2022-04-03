#include "Types.h"
#include "VertexDescription.h"

using namespace Sailor;
using namespace Sailor::RHI;

uint64_t Sailor::RHI::PackVertexAttributeFormat(EFormat format)
{
	switch (format)
	{
	case EFormat::R32G32B32A32_SFLOAT:
		return 0;
	case EFormat::R32G32B32_SFLOAT:
		return 1;
	case EFormat::R32G32_SFLOAT:
		return 2;
	case EFormat::R32_SFLOAT:
		return 3;
	default:
		return 7;
	}
}

EFormat Sailor::RHI::UnpackVertexAttributeFormat(uint64_t attribute)
{
	switch (attribute)
	{
	case 0:
		return EFormat::R32G32B32A32_SFLOAT;
	case 1:
		return EFormat::R32G32B32_SFLOAT;
	case 2:
		return EFormat::R32G32_SFLOAT;
	case 3:
		return EFormat::R32_SFLOAT;
	default:
		return EFormat::UNDEFINED;
	}
}

VertexAttributeBits VertexP3C4::GetVertexAttributeBits()
{
	return (VertexAttributeBits)WriteVertexAttribute(VertexDescription::DefaultPositionLocation, EFormat::R32G32B32_SFLOAT) |
		WriteVertexAttribute(VertexDescription::DefaultColorLocation, EFormat::R32G32B32A32_SFLOAT);
}

VertexAttributeBits VertexP3N3UV2C4::GetVertexAttributeBits()
{
	return (VertexAttributeBits)WriteVertexAttribute(VertexDescription::DefaultPositionLocation, EFormat::R32G32B32_SFLOAT) |
		WriteVertexAttribute(VertexDescription::DefaultNormalLocation, EFormat::R32G32B32_SFLOAT) |
		WriteVertexAttribute(VertexDescription::DefaultTexcoordLocation, EFormat::R32G32_SFLOAT) |
		WriteVertexAttribute(VertexDescription::DefaultColorLocation, EFormat::R32G32B32A32_SFLOAT);
}
