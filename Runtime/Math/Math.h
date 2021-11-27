#pragma once

#include "Core/Defines.h"
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
	const glm::vec3 vec3_Forward = glm::vec3(1, 0, 0);
	const glm::vec3 vec3_Right = glm::vec3(0, 0, 1);

	const glm::vec3 vec3_Back = -vec3_Forward;
	const glm::vec3 vec3_Down = -vec3_Up;
	const glm::vec3 vec3_Left = -vec3_Right;
}

#if defined(min)
#undef min
#define min(a,b) (a < b ? a : b)
#endif

#if defined(max)
#undef max
#define max(a,b) (a < b ? b : a)
#endif