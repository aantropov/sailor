#include "Bounds.h"
#include "Containers/Vector.h"
#include "Memory/LockFreeHeapAllocator.h"

using namespace Sailor;
using namespace Sailor::Math;
using namespace glm;

void Plane::Normalize()
{
	const float mag = glm::length(glm::vec3(m_abcd));
	m_abcd /= mag;
}

void Frustum::ExtractFrustumPlanes(const glm::mat4& matrix, bool bNormalizePlanes)
{
	m_planes[0] = Plane(matrix[3] + matrix[0]);       // left
	m_planes[1] = Plane(matrix[3] - matrix[0]);       // right
	m_planes[2] = Plane(matrix[3] - matrix[1]);       // top
	m_planes[3] = Plane(matrix[3] + matrix[1]);       // bottom
	m_planes[4] = Plane(matrix[3] + matrix[2]);       // near
	m_planes[5] = Plane(matrix[3] - matrix[2]);       // far

	if (bNormalizePlanes)
	{
		for (uint32_t i = 0; i < 6; i++)
		{
			m_planes[i].Normalize();
		}
	}
}

void Frustum::ExtractFrustumPlanes(const Math::Transform& world, float aspect, float fovY, float zNear, float zFar)
{
	const float halfVSide = zFar * tanf(glm::radians(fovY) * .5f);
	const float halfHSide = halfVSide * aspect;
	const glm::vec3 frontMultFar = zFar * world.GetForward();

	const auto& forward = world.GetForward();
	m_planes[4] = Plane(forward, glm::vec3(world.m_position) + forward * zNear);
	m_planes[5] = Plane(-forward, glm::vec3(world.m_position) + forward * zFar);

	const glm::vec3 leftNormal = glm::normalize(glm::cross(frontMultFar - world.GetRight() * halfHSide, world.GetUp()));
	const glm::vec3 rightNormal = glm::normalize(glm::cross(world.GetUp(), frontMultFar + world.GetRight() * halfHSide));
	m_planes[0] = Plane(leftNormal, world.m_position);
	m_planes[1] = Plane(rightNormal, world.m_position);

	const glm::vec3 topNormal = glm::normalize(glm::cross(world.GetRight(), frontMultFar - world.GetUp() * halfVSide));
	const glm::vec3 bottomNormal = glm::normalize(glm::cross(frontMultFar + world.GetUp() * halfVSide, world.GetRight()));
	m_planes[2] = Plane(topNormal, world.m_position);
	m_planes[3] = Plane(bottomNormal, world.m_position);

	for (uint32_t i = 0; i < 6; i++)
	{
		m_planes[i].Normalize();
	}
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

bool Frustum::OverlapsSphere(const Sphere& sphere) const
{
	bool bRes = true;
	for (uint32_t p = 0; p < 6; p++)
	{
		if (m_planes[p][0] * sphere.m_center.x +
			m_planes[p][1] * sphere.m_center.y +
			m_planes[p][2] * sphere.m_center.z +
			m_planes[p][3] < -sphere.m_radius)
		{
			bRes = false;
		}
	}

	return bRes;
}

bool Frustum::ContainsSphere(const Sphere& sphere) const
{
	bool bRes = true;
	for (uint32_t p = 0; p < 6; p++)
	{
		if (m_planes[p][0] * sphere.m_center.x +
			m_planes[p][1] * sphere.m_center.y +
			m_planes[p][2] * sphere.m_center.z +
			m_planes[p][3] < sphere.m_radius)
		{
			bRes = false;
		}
	}

	return bRes;
}

bool Frustum::OverlapsAABB(const AABB& aabb) const
{
	bool bIsInside = true;

	for (uint32_t i = 0; i < 6; i++)
	{
		const float d = max(aabb.m_min.x * m_planes[i].m_abcd.x, aabb.m_max.x * m_planes[i].m_abcd.x)
			+ max(aabb.m_min.y * m_planes[i].m_abcd.y, aabb.m_max.y * m_planes[i].m_abcd.y)
			+ max(aabb.m_min.z * m_planes[i].m_abcd.z, aabb.m_max.z * m_planes[i].m_abcd.z)
			+ m_planes[i].m_abcd.w;

		bIsInside &= d > 0;
	}

	return bIsInside;
}

// We're using SSE intrinsincts to significantly speed-up the culling test
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
		__m128 aabbMinY = _mm_load_ps(pAabbData + 4);
		__m128 aabbMinZ = _mm_load_ps(pAabbData + 8);

		__m128 aabbMaxX = _mm_load_ps(pAabbData + 12);
		__m128 aabbMaxY = _mm_load_ps(pAabbData + 16);
		__m128 aabbMaxZ = _mm_load_ps(pAabbData + 20);

		pAabbData += 24;

		_MM_TRANSPOSE4_PS(aabbMinX, aabbMinY, aabbMinZ, zero);
		_MM_TRANSPOSE4_PS(aabbMaxX, aabbMaxY, aabbMaxZ, zero);

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

void Frustum::ContainsSphere(Sphere* spheres, uint32_t numObjects, int32_t* outResults) const
{
	float* pSpheres = reinterpret_cast<float*>(&spheres[0]);
	int* cullingResults = &outResults[0];

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

	for (i = 0; i < numObjects; i += 4)
	{
		__m128 spheresPosX = _mm_load_ps(pSpheres);
		__m128 spheresPosY = _mm_load_ps(pSpheres + 4);
		__m128 spheresPosZ = _mm_load_ps(pSpheres + 8);
		__m128 spheresRadius = _mm_load_ps(pSpheres + 12);
		pSpheres += 16;

		_MM_TRANSPOSE4_PS(spheresPosX, spheresPosY, spheresPosZ, spheresRadius);

		__m128 intersectionRes = _mm_setzero_ps();

		for (j = 0; j < 6; j++)
		{
			__m128 dotX = _mm_mul_ps(spheresPosX, planesX[j]);
			__m128 dotY = _mm_mul_ps(spheresPosY, planesY[j]);
			__m128 dotZ = _mm_mul_ps(spheresPosZ, planesZ[j]);

			__m128 sumXY = _mm_add_ps(dotX, dotY);
			__m128 sumZW = _mm_add_ps(dotZ, planesD[j]); //z+w

			__m128 distanceToPlane = _mm_add_ps(sumXY, sumZW);
			__m128 planeRes = _mm_cmpge_ps(distanceToPlane, spheresRadius);

			intersectionRes = _mm_and_ps(intersectionRes, planeRes);
		}

		__m128i intersectionResI = _mm_cvtps_epi32(intersectionRes);
		_mm_store_si128((__m128i*) & cullingResults[i], intersectionResI);
	}
}

void Frustum::OverlapsSphere(Sphere* spheres, uint32_t numObjects, int32_t* outResults) const
{
	float* pSpheres = reinterpret_cast<float*>(&spheres[0]);
	int* cullingResults = &outResults[0];

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

	for (i = 0; i < numObjects; i += 4)
	{
		__m128 spheresPosX = _mm_load_ps(pSpheres);
		__m128 spheresPosY = _mm_load_ps(pSpheres + 4);
		__m128 spheresPosZ = _mm_load_ps(pSpheres + 8);
		__m128 spheresRadius = _mm_load_ps(pSpheres + 12);
		pSpheres += 16;

		_MM_TRANSPOSE4_PS(spheresPosX, spheresPosY, spheresPosZ, spheresRadius);

		__m128 spheresNegRadius = _mm_sub_ps(zero_v, spheresRadius);
		__m128 intersectionRes = _mm_setzero_ps();

		for (j = 0; j < 6; j++)
		{
			__m128 dotX = _mm_mul_ps(spheresPosX, planesX[j]);
			__m128 dotY = _mm_mul_ps(spheresPosY, planesY[j]);
			__m128 dotZ = _mm_mul_ps(spheresPosZ, planesZ[j]);

			__m128 sumXY = _mm_add_ps(dotX, dotY);
			__m128 sumZW = _mm_add_ps(dotZ, planesD[j]); //z+w

			__m128 distanceToPlane = _mm_add_ps(sumXY, sumZW);
			__m128 planeRes = _mm_cmple_ps(distanceToPlane, spheresNegRadius);

			intersectionRes = _mm_or_ps(intersectionRes, planeRes);
		}

		__m128i intersectionResI = _mm_cvtps_epi32(intersectionRes);
		_mm_store_si128((__m128i*) & cullingResults[i], intersectionResI);
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