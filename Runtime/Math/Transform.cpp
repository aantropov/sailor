#include "Math.h"
#include "Transform.h"
#include <glm/gtx/quaternion.hpp>

using namespace Sailor;
using namespace Sailor::Math;
using namespace glm;

namespace Sailor::Math
{
	static glm::quat SanitizeRotation(const glm::quat& rotation)
	{
		const glm::vec4 raw(rotation.x, rotation.y, rotation.z, rotation.w);
		if (!AllFinite(raw))
		{
			return quat_Identity;
		}

		const float len2 = glm::dot(raw, raw);
		if (len2 <= std::numeric_limits<float>::epsilon())
		{
			return quat_Identity;
		}

		return rotation * glm::inversesqrt(len2);
	}

	template<>
	SAILOR_API Transform Lerp<Transform>(const Transform& a, const Transform& b, float t)
	{
		return Transform(
			Lerp(a.m_position, b.m_position, t),
			glm::slerp(a.m_rotation, b.m_rotation, t),
			Lerp(a.m_scale, b.m_scale, t));
	}
}

vec3 Transform::GetReciprocalScale() const
{
	return 1.0f / vec3(m_scale);
}

Transform Transform::operator* (const Transform& parent) const
{
	Transform t = *this;

	t.m_position = parent.TransformPosition(t.m_position);
	t.m_rotation = SanitizeRotation(parent.m_rotation) * SanitizeRotation(t.m_rotation);

	t.m_scale.x *= parent.m_scale.x;
	t.m_scale.y *= parent.m_scale.y;
	t.m_scale.z *= parent.m_scale.z;

	return t;
}

Transform& Transform::operator*= (const Transform& parent)
{
	*this = *this * parent;
	return *this;
}

mat4 Transform::Matrix() const
{
	const quat rotation = SanitizeRotation(m_rotation);
	return glm::translate(glm::mat4(1), vec3(m_position)) * glm::toMat4(rotation) * glm::scale(glm::mat4(1), vec3(m_scale));
}

vec4 Transform::TransformPosition(const vec4& position) const
{
	return TransformVector(position) + vec4(vec3(m_position), 0.0f);
}

vec4 Transform::InverseTransformPosition(const vec4& position) const
{
	return InverseTransformVector(position - vec4(vec3(m_position), 0.0f));
}

vec4 Transform::TransformVector(const vec4& vector) const
{
	const quat rotation = SanitizeRotation(m_rotation);
	return rotation * vec4(m_scale.x * vector.x, m_scale.y * vector.y, m_scale.z * vector.z, vector.w);
}

vec4 Transform::InverseTransformVector(const vec4& vector) const
{
	const quat rotation = SanitizeRotation(m_rotation);
	const vec3 unrotated = glm::conjugate(rotation) * vec3(vector);
	return vec4(unrotated * GetReciprocalScale(), vector.w);
}

Transform Transform::Inverse() const
{
	const quat rotation = SanitizeRotation(m_rotation);
	const vec4 invScale = vec4(GetReciprocalScale(), 1.0f);
	const quat invRotation = glm::conjugate(rotation);
	const vec3 invTranslation = -vec3(invScale) * (invRotation * vec3(m_position));

	return Transform(vec4(invTranslation, m_position.w), invRotation, invScale);
}

vec3 Transform::GetForward() const { return glm::rotate(SanitizeRotation(m_rotation), Math::vec3_Forward); }
vec3 Transform::GetRight() const { return glm::rotate(SanitizeRotation(m_rotation), Math::vec3_Right); }
vec3 Transform::GetUp() const { return glm::rotate(SanitizeRotation(m_rotation), Math::vec3_Up); }

const Transform Transform::Identity;

Transform Transform::FromMatrix(const glm::mat4& m)
{
	glm::vec3 translation = glm::vec3(m[3]);

	glm::vec3 scale;
	scale.x = glm::length(glm::vec3(m[0]));
	scale.y = glm::length(glm::vec3(m[1]));
	scale.z = glm::length(glm::vec3(m[2]));

	glm::mat3 rotMat;
	rotMat[0] = scale.x != 0.0f ? glm::vec3(m[0]) / scale.x : glm::vec3(m[0]);
	rotMat[1] = scale.y != 0.0f ? glm::vec3(m[1]) / scale.y : glm::vec3(m[1]);
	rotMat[2] = scale.z != 0.0f ? glm::vec3(m[2]) / scale.z : glm::vec3(m[2]);

	if (glm::determinant(rotMat) < 0.0f)
	{
		glm::length_t reflectionAxis = 0;
		if (scale.y > scale.x)
		{
			reflectionAxis = 1;
		}
		if (scale.z > scale[reflectionAxis])
		{
			reflectionAxis = 2;
		}

		scale[reflectionAxis] = -scale[reflectionAxis];
		rotMat[reflectionAxis] = -rotMat[reflectionAxis];
	}

	glm::quat rotation = glm::quat_cast(rotMat);

	return Transform(glm::vec4(translation, 1.0f), rotation, glm::vec4(scale, 1.0f));
}
