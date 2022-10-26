#pragma once

#include "Transform.h"

#include <glm/glm/glm.hpp>
#include <glm/glm/gtc/quaternion.hpp>
#include <glm/glm/common.hpp>
#include <glm/glm/gtc/matrix_transform.hpp>

using namespace glm;

namespace Sailor::Math
{
	const glm::vec3 vec3_Zero = glm::vec3(0, 0, 0);
	const glm::vec3 vec3_One = glm::vec3(1, 1, 1);

	const glm::vec3 vec3_Up = glm::vec3(0, 1, 0);
	const glm::vec3 vec3_Forward = glm::vec3(0, 0, -1);
	const glm::vec3 vec3_Right = glm::vec3(1, 0, 0);

	const glm::vec3 vec3_Back = -vec3_Forward;
	const glm::vec3 vec3_Down = -vec3_Up;
	const glm::vec3 vec3_Left = -vec3_Right;

	const glm::vec4 vec4_Zero = glm::vec4(0, 0, 0, 0);
	const glm::vec4 vec4_One = glm::vec4(1, 1, 1, 1);

	const glm::vec4 vec4_Up = glm::vec4(0, 1, 0, 0);
	const glm::vec4 vec4_Forward = glm::vec4(0, 0, -1, 0);
	const glm::vec4 vec4_Right = glm::vec4(1, 0, 0, 0);

	const glm::vec4 vec4_Back = -vec4_Forward;
	const glm::vec4 vec4_Down = -vec4_Up;
	const glm::vec4 vec4_Left = -vec4_Right;

	const quat quat_Identity = quat(1.0, 0.0, 0.0, 0.0);
	const mat4 mat4_Identity = mat4(1);

	template<typename T>
	SAILOR_API __forceinline T UpperPowOf2(T v)
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

	template<typename T>
	T Lerp(const T& a, const T& b, float t) { return a + (b - a) * t; }

	SAILOR_API __forceinline glm::mat4 PerspectiveInfiniteRH(float fovRadians, float aspectWbyH, float zNear);
	SAILOR_API __forceinline glm::mat4 PerspectiveRH(float fovRadians, float aspectWbyH, float zNear, float zFar);
}

#if defined(min)
#undef min
#define min(a,b) (a < b ? a : b)
#endif

#if defined(max)
#undef max
#define max(a,b) (a < b ? b : a)
#endif