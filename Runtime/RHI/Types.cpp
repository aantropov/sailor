#include "Types.h"
#include "VertexDescription.h"

using namespace Sailor;
using namespace Sailor::RHI;

bool RHI::IsDepthFormat(ETextureFormat textureFormat)
{
	return IsDepthStencilFormat(textureFormat) ||
		textureFormat == RHI::EFormat::D32_SFLOAT ||
		textureFormat == RHI::EFormat::D16_UNORM;
}

bool RHI::IsDepthStencilFormat(ETextureFormat textureFormat)
{
	return textureFormat == RHI::EFormat::D32_SFLOAT_S8_UINT ||
		textureFormat == RHI::EFormat::D16_UNORM_S8_UINT ||
		textureFormat == RHI::EFormat::D24_UNORM_S8_UINT;
}

uint64_t PackVertexAttributeFormat(EFormat format)
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

EFormat UnpackVertexAttributeFormat(uint64_t attribute)
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

void Sailor::RHI::SetAttributeFormat(VertexAttributeBits& bits, uint32_t shaderBinding, EFormat format)
{
	bits |= (PackVertexAttributeFormat(format) << (shaderBinding * 3ull));
}

EFormat Sailor::RHI::GetAttributeFormat(const VertexAttributeBits& bits, uint32_t shaderBinding)
{
	return (EFormat)UnpackVertexAttributeFormat((bits >> (shaderBinding * 3ull)) & 0x111);
}

VertexAttributeBits VertexP2UV2C1::GetVertexAttributeBits()
{
	VertexAttributeBits bits = 0;
	SetAttributeFormat(bits, RHIVertexDescription::DefaultPositionBinding, EFormat::R32G32_SFLOAT);
	SetAttributeFormat(bits, RHIVertexDescription::DefaultTexcoordBinding, EFormat::R32G32_SFLOAT);
	SetAttributeFormat(bits, RHIVertexDescription::DefaultColorBinding, EFormat::R8G8B8A8_UNORM);
	return bits;
}

VertexAttributeBits VertexP3UV2::GetVertexAttributeBits()
{
	VertexAttributeBits bits = 0;
	SetAttributeFormat(bits, RHIVertexDescription::DefaultPositionBinding, EFormat::R32G32B32_SFLOAT);
	SetAttributeFormat(bits, RHIVertexDescription::DefaultTexcoordBinding, EFormat::R32G32_SFLOAT);
	return bits;
}

VertexAttributeBits VertexP3C4::GetVertexAttributeBits()
{
	VertexAttributeBits bits = 0;
	SetAttributeFormat(bits, RHIVertexDescription::DefaultPositionBinding, EFormat::R32G32B32_SFLOAT);
	SetAttributeFormat(bits, RHIVertexDescription::DefaultColorBinding, EFormat::R32G32B32A32_SFLOAT);
	return bits;
}

VertexAttributeBits VertexP3N3UV2C4::GetVertexAttributeBits()
{
	VertexAttributeBits bits = 0;
	SetAttributeFormat(bits, RHIVertexDescription::DefaultPositionBinding, EFormat::R32G32B32_SFLOAT);
	SetAttributeFormat(bits, RHIVertexDescription::DefaultNormalBinding, EFormat::R32G32B32_SFLOAT);
	SetAttributeFormat(bits, RHIVertexDescription::DefaultTexcoordBinding, EFormat::R32G32_SFLOAT);
	SetAttributeFormat(bits, RHIVertexDescription::DefaultColorBinding, EFormat::R32G32B32A32_SFLOAT);
	return  bits;
}

VertexAttributeBits VertexP3N3T3B3UV2C4::GetVertexAttributeBits()
{
	VertexAttributeBits bits = 0;
	SetAttributeFormat(bits, RHIVertexDescription::DefaultPositionBinding, EFormat::R32G32B32_SFLOAT);
	SetAttributeFormat(bits, RHIVertexDescription::DefaultNormalBinding, EFormat::R32G32B32_SFLOAT);
	SetAttributeFormat(bits, RHIVertexDescription::DefaultTexcoordBinding, EFormat::R32G32_SFLOAT);
	SetAttributeFormat(bits, RHIVertexDescription::DefaultColorBinding, EFormat::R32G32B32A32_SFLOAT);

	SetAttributeFormat(bits, RHIVertexDescription::DefaultTangentBinding, EFormat::R32G32B32_SFLOAT);
	SetAttributeFormat(bits, RHIVertexDescription::DefaultBitangentBinding, EFormat::R32G32B32_SFLOAT);
	return  bits;
}

VertexAttributeBits VertexP3N3T3B3UV2C4I4W4::GetVertexAttributeBits()
{
	VertexAttributeBits bits = 0;
	SetAttributeFormat(bits, RHIVertexDescription::DefaultPositionBinding, EFormat::R32G32B32_SFLOAT);
	SetAttributeFormat(bits, RHIVertexDescription::DefaultNormalBinding, EFormat::R32G32B32_SFLOAT);
	SetAttributeFormat(bits, RHIVertexDescription::DefaultTexcoordBinding, EFormat::R32G32_SFLOAT);
	SetAttributeFormat(bits, RHIVertexDescription::DefaultColorBinding, EFormat::R32G32B32A32_SFLOAT);
	SetAttributeFormat(bits, RHIVertexDescription::DefaultTangentBinding, EFormat::R32G32B32_SFLOAT);
	SetAttributeFormat(bits, RHIVertexDescription::DefaultBitangentBinding, EFormat::R32G32B32_SFLOAT);
	SetAttributeFormat(bits, RHIVertexDescription::DefaultBoneIdsBinding, EFormat::R32G32B32A32_UINT);
	SetAttributeFormat(bits, RHIVertexDescription::DefaultBoneWeightsBinding, EFormat::R32G32B32A32_SFLOAT);
	return bits;
}
