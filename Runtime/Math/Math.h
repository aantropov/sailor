#pragma once

#include "Core/Defines.h"
#include <glm/glm/glm.hpp>
#include <glm/glm/gtc/quaternion.hpp>
#include <glm/glm/common.hpp>
#include <glm/glm/gtc/matrix_transform.hpp>
#include "Transform.h"

using namespace glm;

namespace Sailor::Math
{
	const glm::vec3 vec3_Zero = glm::vec3(0, 0, 0);
	const glm::vec3 vec3_One = glm::vec3(1, 1, 1);

	const glm::vec3 vec3_Up = glm::vec3(0, 1, 0);
	const glm::vec3 vec3_Forward = glm::vec3(1, 0, 0);
	const glm::vec3 vec3_Right = glm::vec3(0, 0, 1);

	const glm::vec3 vec3_Back = -vec3_Forward;
	const glm::vec3 vec3_Down = -vec3_Up;
	const glm::vec3 vec3_Left = -vec3_Right;

	const quat quat_Identity = quat(1.0, 0.0, 0.0, 0.0);
	const mat4 mat4_Identity = mat4(1);

	unsigned long SAILOR_API UpperPowOf2(unsigned long v);

	template<typename T>
	T Lerp(const T& a, const T& b, float t) { return a + (b - a) * t; }

	glm::mat4 PerspectiveInfiniteRH(float fovRadians, float aspectWbyH, float zNear);
}

#if defined(min)
#undef min
#define min(a,b) (a < b ? a : b)
#endif

#if defined(max)
#undef max
#define max(a,b) (a < b ? b : a)
#endif