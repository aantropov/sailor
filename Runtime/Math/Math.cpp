#include "Math/Math.h"

using namespace Sailor;

// Reversed Z matrix
//https://nlguillemot.wordpress.com/2016/12/07/reversed-z-in-opengl/
glm::mat4 Math::PerspectiveInfiniteRH(float fovRadians, float aspectWbyH, float zNear)
{
	float f = 1.0f / tan(fovRadians / 2.0f);
	return glm::mat4(
		f / aspectWbyH, 0.0f, 0.0f, 0.0f,
		0.0f, f, 0.0f, 0.0f,
		0.0f, 0.0f, 0.0f, -1.0f,
		0.0f, 0.0f, zNear, 0.0f);
}

// Reversed Z matrix
glm::mat4 Math::PerspectiveRH(float fovRadians, float aspectWbyH, float zNear, float zFar)
{
	return glm::perspectiveRH(fovRadians, aspectWbyH, zFar, zNear);
}
