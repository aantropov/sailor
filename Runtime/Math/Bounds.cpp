#include "Core/Defines.h"
#include "Math/Bounds.h"
#include <glm/gtx/intersect.hpp>

using namespace Sailor;
using namespace Sailor::Math;
using namespace glm;

void Plane::Normalize()
{
	const float mag = glm::length(glm::vec3(m_abcd));
	m_abcd /= mag;
}

const TVector<glm::vec3>& Frustum::GetCorners() const
{
	return m_corners;
}

void Frustum::ExtractFrustumPlanes(const glm::mat4& projectionViewMatrix, bool bNormalizePlanes)
{
	// TODO: Find why the code doesn't work
	/*m_planes[0] = Plane(matrix[3] + matrix[0]);       // left
	m_planes[1] = Plane(matrix[3] - matrix[0]);       // right
	m_planes[2] = Plane(matrix[3] - matrix[1]);       // top
	m_planes[3] = Plane(matrix[3] + matrix[1]);       // bottom
	m_planes[4] = Plane(matrix[3] + matrix[2]);       // near
	m_planes[5] = Plane(matrix[3] - matrix[2]);       // far
	*/

	CalculateCorners(projectionViewMatrix, true);

	const glm::vec3 right = glm::normalize(m_corners[0] - m_corners[1]);
	const glm::vec3 up = glm::normalize(m_corners[0] - m_corners[3]);
	const glm::vec3 forward = glm::normalize(m_corners[0] - m_corners[4]);

	const glm::vec3 centerFar = 0.5f * (m_corners[0] + m_corners[2]);
	const glm::vec3 centerNear = 0.5f * (m_corners[4] + m_corners[6]);

	const glm::vec3 centerBottom = 0.5f * (m_corners[2] + m_corners[7]);
	const glm::vec3 centerTop = 0.5f * (m_corners[0] + m_corners[5]);

	const glm::vec3 centerLeft = 0.5f * (m_corners[1] + m_corners[6]);
	const glm::vec3 centerRight = 0.5f * (m_corners[0] + m_corners[7]);

	m_planes[4] = Plane(forward, centerNear);
	m_planes[5] = Plane(-forward, centerFar);

	const glm::vec3 leftNormal = glm::normalize(glm::cross(forward, up));
	const glm::vec3 rightNormal = glm::normalize(glm::cross(up, forward));
	m_planes[0] = Plane(leftNormal, centerLeft);
	m_planes[1] = Plane(rightNormal, centerRight);

	const glm::vec3 bottomNormal = glm::normalize(glm::cross(right, forward));
	const glm::vec3 topNormal = glm::normalize(glm::cross(forward, right));
	m_planes[2] = Plane(topNormal, centerTop);
	m_planes[3] = Plane(bottomNormal, centerBottom);

	if (bNormalizePlanes)
	{
		for (uint32_t i = 0; i < 6; i++)
		{
			m_planes[i].Normalize();
		}
	}
}

glm::vec3 Frustum::CalculateCenter() const
{
	glm::vec3 center = glm::vec3(0, 0, 0);
	for (const auto& v : m_corners)
	{
		center += glm::vec3(v);
	}
	return center;
}

glm::mat4 Frustum::CalculateOrthoMatrixByView(const glm::mat4& view, float zMult) const
{
	float minX = std::numeric_limits<float>::max();
	float maxX = std::numeric_limits<float>::lowest();
	float minY = std::numeric_limits<float>::max();
	float maxY = std::numeric_limits<float>::lowest();
	float minZ = std::numeric_limits<float>::max();
	float maxZ = std::numeric_limits<float>::lowest();

	for (const auto& v : m_corners)
	{
		const auto trf = view * glm::vec4(v, 1);
		minX = std::min(minX, trf.x);
		maxX = std::max(maxX, trf.x);
		minY = std::min(minY, trf.y);
		maxY = std::max(maxY, trf.y);
		minZ = std::min(minZ, trf.z);
		maxZ = std::max(maxZ, trf.z);
	}

	// TODO: Redo
	minZ = minZ < 0 ? minZ * zMult : minZ / zMult;
	maxZ = maxZ < 0 ? maxZ / zMult : maxZ * zMult;

	const float zFar = -minZ;
	const float zNear = -maxZ;

	// Viewport settings, we want to handle all shadows with the reversed Z
	const glm::mat4 lightProjection = glm::orthoRH_NO(minX, maxX, minY, maxY, zFar, zNear);

	return lightProjection;
}

void Frustum::CalculateCorners(const glm::mat4& matrix, bool bReverseZ)
{
	const auto inv = glm::inverse(matrix);

	const float reverseZ = bReverseZ ? -1.0f : 1.0f;

	glm::vec4 pt = inv * glm::vec4(1.0f, 1.0f, reverseZ * 1.0f, 1.0f);
	m_corners[0] = vec3(pt / pt.w);

	pt = inv * glm::vec4(-1.0f, 1.0f, reverseZ * 1.0f, 1.0f);
	m_corners[1] = vec3(pt / pt.w);

	pt = inv * glm::vec4(-1.0f, -1.0f, reverseZ * 1.0f, 1.0f);
	m_corners[2] = vec3(pt / pt.w);

	pt = inv * glm::vec4(1.0f, -1.0f, reverseZ * 1.0f, 1.0f);
	m_corners[3] = vec3(pt / pt.w);

	pt = inv * glm::vec4(1.0f, 1.0f, reverseZ * -1.0f, 1.0f);
	m_corners[4] = vec3(pt / pt.w);

	pt = inv * glm::vec4(-1.0f, 1.0f, reverseZ * -1.0f, 1.0f);
	m_corners[5] = vec3(pt / pt.w);

	pt = inv * glm::vec4(-1.0f, -1.0f, reverseZ * -1.0f, 1.0f);
	m_corners[6] = vec3(pt / pt.w);

	pt = inv * glm::vec4(1.0f, -1.0f, reverseZ * -1.0f, 1.0f);
	m_corners[7] = vec3(pt / pt.w);
}

void Frustum::ExtractFrustumPlanes(const glm::mat4& worldMatrix, float aspect, float fovY, float zNear, float zFar)
{
	const float halfVSide = zFar * tanf(glm::radians(fovY) * .5f);
	const float halfHSide = halfVSide * aspect;

	const glm::vec3 right = worldMatrix[0];
	const glm::vec3 up = worldMatrix[1];
	const glm::vec3 forward = -worldMatrix[2];
	const glm::vec3 pos = worldMatrix[3];

	const glm::vec3 frontMultFar = zFar * forward;

	m_planes[4] = Plane(forward, pos + forward * zNear);
	m_planes[5] = Plane(-forward, pos + forward * zFar);

	const glm::vec3 leftNormal = glm::normalize(glm::cross(frontMultFar - right * halfHSide, up));
	const glm::vec3 rightNormal = glm::normalize(glm::cross(up, frontMultFar + right * halfHSide));
	m_planes[0] = Plane(leftNormal, pos);
	m_planes[1] = Plane(rightNormal, pos);

	const glm::vec3 bottomNormal = glm::normalize(glm::cross(right, frontMultFar - up * halfVSide));
	const glm::vec3 topNormal = glm::normalize(glm::cross(frontMultFar + up * halfVSide, right));
	m_planes[2] = Plane(topNormal, pos);
	m_planes[3] = Plane(bottomNormal, pos);

	for (uint32_t i = 0; i < 6; i++)
	{
		m_planes[i].Normalize();
	}

	// Analytically calculate the corners
	const glm::vec3 farEnd(0, 0, -zFar);
	const glm::vec3 endSizeHorizontal(halfHSide, 0, 0);
	const glm::vec3 endSizeVertical(0, halfVSide, 0);

	m_corners[0] = worldMatrix * glm::vec4(farEnd + endSizeHorizontal + endSizeVertical, 1);
	m_corners[1] = worldMatrix * glm::vec4(farEnd - endSizeHorizontal + endSizeVertical, 1);
	m_corners[2] = worldMatrix * glm::vec4(farEnd - endSizeHorizontal - endSizeVertical, 1);
	m_corners[3] = worldMatrix * glm::vec4(farEnd + endSizeHorizontal - endSizeVertical, 1);

	const float halfVSideNear = zNear * tanf(glm::radians(fovY) * .5f);
	const float halfHSideNear = halfVSideNear * aspect;

	const glm::vec3 startPoint = glm::vec3(0, 0, -zNear);
	const glm::vec3 startSizeX(halfHSideNear, 0, 0);
	const glm::vec3 startSizeY(0, halfVSideNear, 0);

	m_corners[4] = worldMatrix * glm::vec4(startPoint + startSizeX + startSizeY, 1);
	m_corners[5] = worldMatrix * glm::vec4(startPoint - startSizeX + startSizeY, 1);
	m_corners[6] = worldMatrix * glm::vec4(startPoint - startSizeX - startSizeY, 1);
	m_corners[7] = worldMatrix * glm::vec4(startPoint + startSizeX - startSizeY, 1);
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

float AABB::Volume() const
{
	vec3 e = m_max - m_min;
	return e.x * e.y * e.z;
}

float AABB::Area() const
{
	const vec3 e = m_max - m_min;
	return 2.0f * (e.x * e.y + e.y * e.z + e.z * e.x);
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

Sphere AABB::ToSphere() const
{
	const vec3& extents = GetExtents();
	const float radius = glm::length(extents);

	return Sphere(GetCenter(), radius);
}

AABB::AABB(glm::vec3 center, glm::vec3 extents)
{
	m_min = center - extents;
	m_max = center + extents;
}

void AABB::Apply(const glm::mat4& transformMatrix)
{
	TVector<glm::vec3> points;
	GetPoints(points);

	m_max = glm::vec3(std::numeric_limits<float>::min());
	m_min = glm::vec3(std::numeric_limits<float>::max());

	for (auto& point : points)
	{
		const auto& transformed = transformMatrix * glm::vec4(point, 1.0f);
		Extend(transformed);
	}
}

float Math::Triangle::SquareArea() const
{
	float a = length(m_vertices[0] - m_vertices[1]);
	float b = length(m_vertices[0] - m_vertices[2]);
	float c = length(m_vertices[2] - m_vertices[1]);

	float s = (a + b + c) / 2;
	return s * (s - a) * (s - b) * (s - c);
}

float Math::Triangle::Area() const
{
	return sqrt(SquareArea());
}

bool Math::IntersectRayTriangle(const Ray& ray, const Triangle& tri, RaycastHit& outRaycastHit, float maxRayLength)
{
	outRaycastHit = RaycastHit();
	outRaycastHit.m_rayLenght = maxRayLength;

	float distance = std::numeric_limits<float>::max();
	vec2 baryPosition{};

	if (bool res = Math::IntersectRayTriangle(ray.GetOrigin(), ray.GetDirection(), tri.m_vertices[0], tri.m_vertices[1], tri.m_vertices[2], baryPosition, distance))
	{
		if (distance < maxRayLength && distance > -0.0000001f)
		{
			outRaycastHit.m_barycentricCoordinate = vec3(1.0f - baryPosition.x - baryPosition.y, baryPosition.x, baryPosition.y);
			outRaycastHit.m_point = ray.GetOrigin() + ray.GetDirection() * distance;
			outRaycastHit.m_normal = outRaycastHit.m_barycentricCoordinate.x * tri.m_normals[0] +
				outRaycastHit.m_barycentricCoordinate.y * tri.m_normals[1] +
				outRaycastHit.m_barycentricCoordinate.z * tri.m_normals[2];
			outRaycastHit.m_triangleIndex = 0;
			outRaycastHit.m_rayLenght = distance;
		}
	}

	return outRaycastHit.HasIntersection();
}

bool Math::IntersectRayTriangle(const Ray& ray, const TVector<Triangle>& tris, RaycastHit& outRaycastHit, float maxRayLength)
{
	check(false);
	return false;
}

/*
// This is the famous Möller–Trumbore intersection algorithm, which is still pretty close to ‘as fast as possible’.
// https://en.wikipedia.org/wiki/M%C3%B6ller%E2%80%93Trumbore_intersection_algorithm
void IntersectRayTriangle(Ray& ray, const Triangle& tri)
{
	const vec3 edge1 = tri.m_vertices[1] - tri.m_vertices[0];
	const vec3 edge2 = tri.m_vertices[2] - tri.m_vertices[0];
	const vec3 h = cross(ray.GetDirection(), edge2);
	const float a = dot(edge1, h);

	if (a > -0.0001f && a < 0.0001f)
	{
		// ray parallel to triangle
		return;
	}

	const float f = 1 / a;
	const vec3 s = ray.GetOrigin() - tri.m_vertices[0];
	const float u = f * dot(s, h);

	if (u < 0 || u > 1)
	{
		return;
	}

	const vec3 q = cross(s, edge1);
	const float v = f * dot(ray.GetDirection(), q);

	if (v < 0 || u + v > 1)
	{
		return;
	}

	const float t = f * dot(edge2, q);
	if (t > 0.0001f)
	{
		ray.m_t = min(ray.m_t, t);
	}
}*/

float Math::IntersectRayAABB(const Ray& ray, const glm::vec3& bmin, const glm::vec3& bmax, float maxRayLength)
{
	__m128 _bmin = _mm_setr_ps(bmin.x, bmin.y, bmin.z, 0.f);
	__m128 _bmax = _mm_setr_ps(bmax.x, bmax.y, bmax.z, 0.f);
	__m128 t1 = _mm_mul_ps(_mm_sub_ps(_bmin, ray.GetOrigin4()), ray.GetReciprocalDirection4());
	__m128 t2 = _mm_mul_ps(_mm_sub_ps(_bmax, ray.GetOrigin4()), ray.GetReciprocalDirection4());

	float vmax[4], vmin[4];
	_mm_store_ps(vmax, _mm_max_ps(t1, t2));
	_mm_store_ps(vmin, _mm_min_ps(t1, t2));

	float tmax = glm::min(vmax[0], glm::min(vmax[1], vmax[2]));
	float tmin = glm::max(vmin[0], glm::max(vmin[1], vmin[2]));

	if (tmax >= tmin && tmin < maxRayLength && tmax > 0.f)
	{
		return tmin;
	}

	return std::numeric_limits<float>::max();
}
