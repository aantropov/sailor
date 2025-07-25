#include "Math.h"
#include "Transform.h"
#include <glm/gtx/quaternion.hpp>

using namespace Sailor;
using namespace Sailor::Math;
using namespace glm;

namespace Sailor::Math
{
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
	t.m_rotation = t.m_rotation * parent.m_rotation;

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
	return glm::translate(glm::mat4(1), vec3(m_position)) * glm::toMat4(m_rotation) * glm::scale(glm::mat4(1), vec3(m_scale));
}

vec4 Transform::TransformPosition(const vec4& position) const
{
	return m_rotation * vec4(m_scale.x * position.x, m_scale.y * position.y, m_scale.z * position.z, position.w) + m_position;
}

vec4 Transform::InverseTransformPosition(const vec4& position) const
{
	return glm::toMat4(glm::inverse(m_rotation)) * (position - m_position);
}

vec4 Transform::TransformVector(const vec4& vector) const
{
	return m_rotation * vec4(m_scale.x * vector.x, m_scale.y * vector.y, m_scale.z * vector.z, vector.w);
}

vec4 Transform::InverseTransformVector(const vec4& vector) const
{
	vec3 scale = GetReciprocalScale();
	return glm::inverse(m_rotation) * vec4(scale.x * vector.x, scale.y * vector.y, scale.z * vector.z, vector.w);
}

Transform Transform::Inverse() const
{
	const vec4 invScale = vec4(GetReciprocalScale(), 1.0f);
	const quat invRotation = glm::inverse(m_rotation);
	const vec4 scaledTranslation = invRotation * (invScale * m_position);

	return Transform(-scaledTranslation, invRotation, invScale);
}

vec3 Transform::GetForward() const { return glm::rotate(m_rotation, Math::vec3_Forward); }
vec3 Transform::GetRight() const { return glm::rotate(m_rotation, Math::vec3_Right); }
vec3 Transform::GetUp() const { return glm::rotate(m_rotation, Math::vec3_Up); }

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
	glm::quat rotation = glm::quat_cast(rotMat);

	return Transform(glm::vec4(translation, 1.0f), rotation, glm::vec4(scale, 1.0f));
}
