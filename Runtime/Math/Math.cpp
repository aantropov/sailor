#include "Math.h"

using namespace Sailor;
using namespace Sailor::Math;

unsigned long Sailor::Math::UpperPowOf2(unsigned long v)
{
	v--;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	v++;
	return v;
}

glm::mat4 Sailor::Math::PerspectiveInfiniteRH(float fovRadians, float aspectWbyH, float zNear)
{
	float f = 1.0f / tan(fovRadians / 2.0f);
	return glm::mat4(
		f / aspectWbyH, 0.0f, 0.0f, 0.0f,
		0.0f, f, 0.0f, 0.0f,
		0.0f, 0.0f, 0.0f, -1.0f,
		0.0f, 0.0f, zNear, 0.0f);
}