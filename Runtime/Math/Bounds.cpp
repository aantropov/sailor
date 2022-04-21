#include "Bounds.h"
#include "Containers/Vector.h"
#include "Memory/LockFreeHeapAllocator.h"

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

bool Frustum::ContainsPoint(const glm::vec3& point) const
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

bool Frustum::ContainsSphere(const Sphere& sphere) const
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

bool Frustum::OverlapsAABB(const AABB& aabb) const
{
	bool bInside = true;

	for (uint32_t i = 0; i < 6; i++)
	{
		float d = max(aabb.m_min.x * m_planes[i].m_abcd.x, aabb.m_max.x * m_planes[i].m_abcd.x)
			+ max(aabb.m_min.y * m_planes[i].m_abcd.y, aabb.m_max.y * m_planes[i].m_abcd.y)
			+ max(aabb.m_min.z * m_planes[i].m_abcd.z, aabb.m_max.z * m_planes[i].m_abcd.z)
			+ m_planes[i].m_abcd.w;

		bInside &= d > 0;
	}

	return bInside;

}

// https://gamedev.ru/code/articles/FrustumCulling
void Frustum::OverlapsAABB(AABB* aabb, uint32_t numObjects, int32_t* outResults) const
{
	float* pAabbData = reinterpret_cast<float*>(&aabb[0]);
	int32_t* cullingResSse = &outResults[0];

	__m128 zero_v = _mm_setzero_ps();
	__m128 planesX[6];
	__m128 planesY[6];
	__m128 planesZ[6];
	__m128 planesD[6];

	uint32_t i, j;
	for (i = 0; i < 6; i++)
	{
		planesX[i] = _mm_set1_ps(m_planes[i].m_abcd.x);
		planesY[i] = _mm_set1_ps(m_planes[i].m_abcd.y);
		planesZ[i] = _mm_set1_ps(m_planes[i].m_abcd.z);
		planesD[i] = _mm_set1_ps(m_planes[i].m_abcd.w);
	}

	__m128 zero = _mm_setzero_ps();
	for (i = 0; i < numObjects; i += 4)
	{
		__m128 aabbMinX = _mm_load_ps(pAabbData);
		__m128 aabbMinY = _mm_load_ps(pAabbData + 8);
		__m128 aabbMinZ = _mm_load_ps(pAabbData + 16);
		__m128 aabbMinW = _mm_load_ps(pAabbData + 24);

		__m128 aabbMaxX = _mm_load_ps(pAabbData + 4);
		__m128 aabbMaxY = _mm_load_ps(pAabbData + 12);
		__m128 aabbMaxZ = _mm_load_ps(pAabbData + 20);
		__m128 aabbMaxW = _mm_load_ps(pAabbData + 28);

		pAabbData += 32;

		_MM_TRANSPOSE4_PS(aabbMinX, aabbMinY, aabbMinZ, aabbMinW);
		_MM_TRANSPOSE4_PS(aabbMaxX, aabbMaxY, aabbMaxZ, aabbMaxW);

		__m128 intersectionRes = _mm_setzero_ps();
		for (j = 0; j < 6; j++)
		{
			__m128 aabbMin_frustumPlaneX = _mm_mul_ps(aabbMinX, planesX[j]);
			__m128 aabbMin_frustumPlaneY = _mm_mul_ps(aabbMinY, planesY[j]);
			__m128 aabbMin_frustumPlaneZ = _mm_mul_ps(aabbMinZ, planesZ[j]);

			__m128 aabbMax_frustumPlaneX = _mm_mul_ps(aabbMaxX, planesX[j]);
			__m128 aabbMax_frustumPlaneY = _mm_mul_ps(aabbMaxY, planesY[j]);
			__m128 aabbMax_frustumPlaneZ = _mm_mul_ps(aabbMaxZ, planesZ[j]);

			__m128 resX = _mm_max_ps(aabbMin_frustumPlaneX, aabbMax_frustumPlaneX);
			__m128 resY = _mm_max_ps(aabbMin_frustumPlaneY, aabbMax_frustumPlaneY);
			__m128 resZ = _mm_max_ps(aabbMin_frustumPlaneZ, aabbMax_frustumPlaneZ);

			__m128 sumXy = _mm_add_ps(resX, resY);
			__m128 sumZw = _mm_add_ps(resZ, planesD[j]);
			__m128 distanceToPlane = _mm_add_ps(sumXy, sumZw);

			__m128 planeRes = _mm_cmple_ps(distanceToPlane, zero);
			intersectionRes = _mm_or_ps(intersectionRes, planeRes);
		}

		__m128i intersectionResI = _mm_cvtps_epi32(intersectionRes);
		_mm_store_si128((__m128i*) & cullingResSse[i], intersectionResI);
	}
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

AABB::AABB(glm::vec3 center, glm::vec3 extents)
{
	m_min = center - extents;
	m_max = center + extents;
}