#pragma once

#include <glm/glm/glm.hpp>
#include <glm/glm/vec4.hpp>
#include <glm/glm/gtc/quaternion.hpp>
#include <glm/glm/common.hpp>
#include <glm/glm/gtc/matrix_transform.hpp>

using namespace glm;

namespace Sailor::Math
{
	struct Transform
	{
	public:

		glm::vec4 m_position;
		glm::quat m_rotation;
		glm::vec4 m_scale;

		SAILOR_API Transform operator*(const Transform& parent) const;
		SAILOR_API Transform& operator*=(const Transform& parent);

		SAILOR_API vec4 TransformPosition(const vec4& position) const;
		SAILOR_API vec4 InverseTransformPosition(const vec4& position) const;

		SAILOR_API vec4 TransformVector(const vec4& vector) const;
		SAILOR_API vec4 InverseTransformVector(const vec4& vector) const;

		SAILOR_API mat4 Matrix() const;

		SAILOR_API vec3 GetReciprocalScale() const;

		SAILOR_API Transform Inverse() const;

		SAILOR_API Transform(vec4 pos = vec4(0.0f, 0.0f, 0.0f, 0.0f),
			quat rot = quat(1.0, 0.0, 0.0, 0.0),
			vec4 scale = vec4(1.0f, 1.0f, 1.0f, 1.0f)) : m_position(pos), m_rotation(rot), m_scale(scale) {}

		static const Transform Identity;
	};

	Transform SAILOR_API Lerp(const Transform& a, const Transform& b, float t);
}