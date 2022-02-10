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

		Transform operator*(const Transform& parent) const;
		Transform& operator*=(const Transform& parent);

		vec4 TransformPosition(const vec4& position) const;
		vec4 InverseTransformPosition(const vec4& position) const;

		vec4 TransformVector(const vec4& vector) const;
		vec4 InverseTransformVector(const vec4& vector) const;

		mat4 Matrix() const;

		vec3 GetReciprocalScale() const;

		Transform Inverse() const;

		Transform(vec4 pos = vec4(0.0f, 0.0f, 0.0f, 0.0f),
			quat rot = quat(1.0, 0.0, 0.0, 0.0),
			vec4 scale = vec4(1.0f, 1.0f, 1.0f, 1.0f)) : m_position(pos), m_rotation(rot), m_scale(scale) {}

		static SAILOR_API const Transform Identity;
	};

	Transform Lerp(const Transform& a, const Transform& b, float t);
}