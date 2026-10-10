#include "MaterialUtils.h"

using namespace Sailor;
using namespace Sailor::Math;
using namespace Sailor::Raytracing;

uint Raytracing::PackVec3ToByte(vec3 v)
{
	vec3 clamped = clamp(v, 0.0f, 1.0f);

	uint r = uint(clamped.r * 3.0f + 0.5f);
	uint g = uint(clamped.g * 3.0f + 0.5f);
	uint b = uint(clamped.b * 3.0f + 0.5f);

	// bbbb gggg rrrr
	return (b << 4) | (g << 2) | r;
}

vec3 Raytracing::UnpackByteToVec3(uint byte)
{
	uint b = (byte >> 4) & 0x3;
	uint g = (byte >> 2) & 0x3;
	uint r = byte & 0x3;

	return vec3(float(r) / 3.0f, float(g) / 3.0f, float(b) / 3.0f);
}

void Raytracing::GenerateTangentBitangent(
	vec3& outTangent,
	vec3& outBitangent,
	const vec3* vert,
	const vec2* uv)
{
	vec3 edge1 = vert[1] - vert[0];
	vec3 edge2 = vert[2] - vert[0];

	vec2 deltaUV1 = uv[1] - uv[0];
	vec2 deltaUV2 = uv[2] - uv[0];

	float denominator = deltaUV1.x * deltaUV2.y - deltaUV2.x * deltaUV1.y;
	if (abs(denominator) < 1e-6f)
	{
		return;
	}

	float f = 1.0f / denominator;

	outTangent = SafeNormalize(vec3(
		f * (deltaUV2.y * edge1.x - deltaUV1.y * edge2.x),
		f * (deltaUV2.y * edge1.y - deltaUV1.y * edge2.y),
		f * (deltaUV2.y * edge1.z - deltaUV1.y * edge2.z)
	));

	outBitangent = SafeNormalize(vec3(
		f * (-deltaUV2.x * edge1.x + deltaUV1.x * edge2.x),
		f * (-deltaUV2.x * edge1.y + deltaUV1.x * edge2.y),
		f * (-deltaUV2.x * edge1.z + deltaUV1.x * edge2.z)
	));
}
