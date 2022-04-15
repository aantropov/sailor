#include "Bounds.h"

using namespace Sailor;
using namespace Sailor::Math;
using namespace glm;

void Frustum::ExtractFrustumPlanes(const glm::mat4& matrix)
{
	// Left clipping plane
	m_planes[0][0] = matrix[3][0] + matrix[0][0];
	m_planes[0][1] = matrix[3][1] + matrix[0][1];
	m_planes[0][2] = matrix[3][2] + matrix[0][2];
	m_planes[0][3] = matrix[3][3] + matrix[0][3];
	// Right clipping plane
	m_planes[1][0] = matrix[3][0] - matrix[0][0];
	m_planes[1][1] = matrix[3][1] - matrix[0][1];
	m_planes[1][2] = matrix[3][2] - matrix[0][2];
	m_planes[1][3] = matrix[3][3] - matrix[0][3];
	// Top clipping plane
	m_planes[2][0] = matrix[3][0] - matrix[1][0];
	m_planes[2][1] = matrix[3][1] - matrix[1][1];
	m_planes[2][2] = matrix[3][2] - matrix[1][2];
	m_planes[2][3] = matrix[3][3] - matrix[1][3];
	// Bottom clipping plane
	m_planes[3][0] = matrix[3][0] + matrix[1][0];
	m_planes[3][1] = matrix[3][1] + matrix[1][1];
	m_planes[3][2] = matrix[3][2] + matrix[1][2];
	m_planes[3][3] = matrix[3][3] + matrix[1][3];
	// Near clipping plane
	m_planes[4][0] = matrix[3][0] + matrix[2][0];
	m_planes[4][1] = matrix[3][1] + matrix[2][1];
	m_planes[4][2] = matrix[3][2] + matrix[2][2];
	m_planes[4][3] = matrix[3][3] + matrix[2][3];
	// Far clipping plane
	m_planes[5][0] = matrix[3][0] - matrix[2][0];
	m_planes[5][1] = matrix[3][1] - matrix[2][1];
	m_planes[5][2] = matrix[3][2] - matrix[2][2];
	m_planes[5][3] = matrix[3][3] - matrix[2][3];
}

bool Frustum::CheckPoint(const glm::vec3& point) const
{
	for (uint32_t p = 0; p < 6; p++)
	{
		if (m_planes[p][0] * point.x +
			m_planes[p][1] * point.y +
			m_planes[p][2] * point.z +
			m_planes[p][3] <= 0.0f)
		{
			return false;
		}
	}

	return true;
}

bool Frustum::CheckSphere(const Sphere& sphere) const
{
	for (uint32_t p = 0; p < 6; p++)
	{
		if (m_planes[p][0] * sphere.m_center.x +
			m_planes[p][1] * sphere.m_center.y +
			m_planes[p][2] * sphere.m_center.z +
			m_planes[p][3] <= -sphere.m_radius)
		{
			return false;
		}
	}

	return true;
}

void AABB::Extend(const AABB& inner)
{
	m_min = glm::min(inner.m_min, m_min);
	m_max = glm::max(inner.m_max, m_max);
}

void AABB::Extend(const glm::vec3& inner)
{
	m_min = glm::min(inner, m_min);
	m_max = glm::max(inner, m_max);
}

glm::vec3 AABB::GetCenter() const
{
	return (m_min + m_max) * 0.5f;
}

glm::vec3 AABB::GetExtents() const
{
	return (m_max - m_min) * 0.5f;
}
