#include "Math.h"
#include "Transform.h"
#include <glm/glm/gtx/quaternion.hpp>

using namespace Sailor;
using namespace Sailor::Math;
using namespace glm;

Transform Lerp(const Transform& a, const Transform& b, float t)
{
	return Transform(Lerp(a.m_position, b.m_position, t), glm::lerp(a.m_rotation, b.m_rotation, t), Lerp(a.m_scale, b.m_scale, t));
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
	mat4 res(1.0f);
	res = glm::translate(res, vec3(m_position));
	res *= glm::toMat4(-m_rotation);
	res = glm::scale(res, vec3(m_scale));

	return res;
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

const Transform Transform::Identity;