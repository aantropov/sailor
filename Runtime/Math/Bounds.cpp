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
	// GLM indexes columns; clipping inequalities use projection-view rows.
	const glm::mat4 rows = glm::transpose(projectionViewMatrix);
	m_planes[0] = Plane(rows[3] + rows[0]);
	m_planes[1] = Plane(rows[3] - rows[0]);
	m_planes[2] = Plane(rows[3] - rows[1]);
	m_planes[3] = Plane(rows[3] + rows[1]);
	m_planes[4] = Plane(rows[3] - rows[2]);
	m_planes[5] = Plane(rows[2]);

	CalculateCorners(projectionViewMatrix, true);

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
	return center / (float)m_corners.Num();
}

glm::mat4 Frustum::CalculateOrthoMatrixByView(const glm::mat4& view, float zMult, glm::ivec2 shadowMapResolution, float zSourceExtension) const
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

	if (shadowMapResolution.x > 0 && shadowMapResolution.y > 0)
	{
		const glm::vec3 worldCenter = CalculateCenter();
		const glm::vec3 lightSpaceCenter = glm::vec3(view * glm::vec4(worldCenter, 1.0f));
		float radius = 0.0f;
		for (const glm::vec3& corner : m_corners)
		{
			radius = (std::max)(radius, glm::length(corner - worldCenter));
		}

		// A sphere keeps the orthographic extent invariant while the camera rotates.
		// Snap its light-space center to the texel grid so camera translations do
		// not make an otherwise static shadow crawl between shadow-map texels.
		if (radius > std::numeric_limits<float>::epsilon())
		{
			// Snapping can move the projection by half a texel. Keep the original
			// receiver sphere plus a complete two-texel PCF footprint inside the
			// map on both axes, otherwise every cascade exposes a border strip.
			constexpr float shadowGuardTexels = 3.0f;
			const float safeResolutionX = (std::max)(
				(float)shadowMapResolution.x - 2.0f * shadowGuardTexels,
				1.0f);
			const float safeResolutionY = (std::max)(
				(float)shadowMapResolution.y - 2.0f * shadowGuardTexels,
				1.0f);
			const float guardedRadius = radius * (std::max)(
				(float)shadowMapResolution.x / safeResolutionX,
				(float)shadowMapResolution.y / safeResolutionY);
			const float diameter = 2.0f * guardedRadius;
			const float texelSizeX = diameter / (float)shadowMapResolution.x;
			const float texelSizeY = diameter / (float)shadowMapResolution.y;
			const float snappedCenterX = std::round(lightSpaceCenter.x / texelSizeX) * texelSizeX;
			const float snappedCenterY = std::round(lightSpaceCenter.y / texelSizeY) * texelSizeY;
			minX = snappedCenterX - guardedRadius;
			maxX = snappedCenterX + guardedRadius;
			minY = snappedCenterY - guardedRadius;
			maxY = snappedCenterY + guardedRadius;
		}
	}

	// Extend the fitted depth interval relative to its own center. Scaling the
	// absolute coordinates made the cascade depend on which side of the light's
	// origin the camera happened to be on.
	const float zPadding = 0.5f * (maxZ - minZ) * (zMult - 1.0f);
	minZ -= zPadding;
	// In light-view space, larger Z is toward the directional-light source.
	// Casters behind the camera can still project into the receiver slice, so
	// reserve a world-space range on that side without expanding cascade XY.
	maxZ += zPadding + (std::max)(zSourceExtension, 0.0f);

	const float zFar = -minZ;
	const float zNear = -maxZ;

	// Viewport settings, we want to handle all shadows with the reversed Z
	const glm::mat4 lightProjection = glm::orthoRH_ZO(minX, maxX, minY, maxY, zFar, zNear);

	return lightProjection;
}

void Frustum::CalculateCorners(const glm::mat4& matrix, bool bReverseZ)
{
	const auto inv = glm::inverse(matrix);

	const float farDepth = bReverseZ ? 0.0f : 1.0f;
	const float nearDepth = bReverseZ ? 1.0f : 0.0f;

	glm::vec4 pt = inv * glm::vec4(1.0f, 1.0f, farDepth, 1.0f);
	m_corners[0] = vec3(pt / pt.w);

	pt = inv * glm::vec4(-1.0f, 1.0f, farDepth, 1.0f);
	m_corners[1] = vec3(pt / pt.w);

	pt = inv * glm::vec4(-1.0f, -1.0f, farDepth, 1.0f);
	m_corners[2] = vec3(pt / pt.w);

	pt = inv * glm::vec4(1.0f, -1.0f, farDepth, 1.0f);
	m_corners[3] = vec3(pt / pt.w);

	pt = inv * glm::vec4(1.0f, 1.0f, nearDepth, 1.0f);
	m_corners[4] = vec3(pt / pt.w);

	pt = inv * glm::vec4(-1.0f, 1.0f, nearDepth, 1.0f);
	m_corners[5] = vec3(pt / pt.w);

	pt = inv * glm::vec4(-1.0f, -1.0f, nearDepth, 1.0f);
	m_corners[6] = vec3(pt / pt.w);

	pt = inv * glm::vec4(1.0f, -1.0f, nearDepth, 1.0f);
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

void Frustum::OverlapsAABB(AABB* aabb, uint32_t numObjects, int32_t* outResults) const
{
	uint32_t i = 0;
#if SAILOR_USE_X86_SIMD
	for (; numObjects - i >= 4; i += 4)
	{
		const __m128 minX = _mm_setr_ps(aabb[i].m_min.x, aabb[i + 1].m_min.x, aabb[i + 2].m_min.x, aabb[i + 3].m_min.x);
		const __m128 minY = _mm_setr_ps(aabb[i].m_min.y, aabb[i + 1].m_min.y, aabb[i + 2].m_min.y, aabb[i + 3].m_min.y);
		const __m128 minZ = _mm_setr_ps(aabb[i].m_min.z, aabb[i + 1].m_min.z, aabb[i + 2].m_min.z, aabb[i + 3].m_min.z);
		const __m128 maxX = _mm_setr_ps(aabb[i].m_max.x, aabb[i + 1].m_max.x, aabb[i + 2].m_max.x, aabb[i + 3].m_max.x);
		const __m128 maxY = _mm_setr_ps(aabb[i].m_max.y, aabb[i + 1].m_max.y, aabb[i + 2].m_max.y, aabb[i + 3].m_max.y);
		const __m128 maxZ = _mm_setr_ps(aabb[i].m_max.z, aabb[i + 1].m_max.z, aabb[i + 2].m_max.z, aabb[i + 3].m_max.z);
		const __m128 zero = _mm_setzero_ps();
		__m128 rejected = zero;
		for (uint32_t p = 0; p < 6; ++p)
		{
			const __m128 x = _mm_set1_ps(m_planes[p].m_abcd.x);
			const __m128 y = _mm_set1_ps(m_planes[p].m_abcd.y);
			const __m128 z = _mm_set1_ps(m_planes[p].m_abcd.z);
			// MAXPS chooses its second operand on ties/unordered inputs, matching scalar max(min, max).
			__m128 distance = _mm_max_ps(_mm_mul_ps(maxX, x), _mm_mul_ps(minX, x));
			distance = _mm_add_ps(distance, _mm_max_ps(_mm_mul_ps(maxY, y), _mm_mul_ps(minY, y)));
			distance = _mm_add_ps(distance, _mm_max_ps(_mm_mul_ps(maxZ, z), _mm_mul_ps(minZ, z)));
			distance = _mm_add_ps(distance, _mm_set1_ps(m_planes[p].m_abcd.w));
			rejected = _mm_or_ps(rejected, _mm_cmpngt_ps(distance, zero));
		}
		const __m128i results = _mm_and_si128(_mm_castps_si128(rejected), _mm_set1_epi32(1));
		_mm_storeu_si128(reinterpret_cast<__m128i*>(outResults + i), results);
	}
#endif
	for (; i < numObjects; ++i)
	{
		outResults[i] = OverlapsAABB(aabb[i]) ? 0 : 1;
	}
}

void Frustum::ContainsSphere(Sphere* spheres, uint32_t numObjects, int32_t* outResults) const
{
	uint32_t i = 0;
#if SAILOR_USE_X86_SIMD
	for (; numObjects - i >= 4; i += 4)
	{
		const __m128 centerX = _mm_setr_ps(spheres[i].m_center.x, spheres[i + 1].m_center.x, spheres[i + 2].m_center.x, spheres[i + 3].m_center.x);
		const __m128 centerY = _mm_setr_ps(spheres[i].m_center.y, spheres[i + 1].m_center.y, spheres[i + 2].m_center.y, spheres[i + 3].m_center.y);
		const __m128 centerZ = _mm_setr_ps(spheres[i].m_center.z, spheres[i + 1].m_center.z, spheres[i + 2].m_center.z, spheres[i + 3].m_center.z);
		const __m128 radius = _mm_setr_ps(spheres[i].m_radius, spheres[i + 1].m_radius, spheres[i + 2].m_radius, spheres[i + 3].m_radius);
		__m128 rejected = _mm_setzero_ps();
		for (uint32_t p = 0; p < 6; ++p)
		{
			__m128 distance = _mm_mul_ps(_mm_set1_ps(m_planes[p].m_abcd.x), centerX);
			distance = _mm_add_ps(distance, _mm_mul_ps(_mm_set1_ps(m_planes[p].m_abcd.y), centerY));
			distance = _mm_add_ps(distance, _mm_mul_ps(_mm_set1_ps(m_planes[p].m_abcd.z), centerZ));
			distance = _mm_add_ps(distance, _mm_set1_ps(m_planes[p].m_abcd.w));
			rejected = _mm_or_ps(rejected, _mm_cmplt_ps(distance, radius));
		}
		const __m128i results = _mm_andnot_si128(_mm_castps_si128(rejected), _mm_set1_epi32(1));
		_mm_storeu_si128(reinterpret_cast<__m128i*>(outResults + i), results);
	}
#endif
	for (; i < numObjects; ++i)
	{
		outResults[i] = ContainsSphere(spheres[i]) ? 1 : 0;
	}
}

void Frustum::OverlapsSphere(Sphere* spheres, uint32_t numObjects, int32_t* outResults) const
{
	uint32_t i = 0;
#if SAILOR_USE_X86_SIMD
	for (; numObjects - i >= 4; i += 4)
	{
		const __m128 centerX = _mm_setr_ps(spheres[i].m_center.x, spheres[i + 1].m_center.x, spheres[i + 2].m_center.x, spheres[i + 3].m_center.x);
		const __m128 centerY = _mm_setr_ps(spheres[i].m_center.y, spheres[i + 1].m_center.y, spheres[i + 2].m_center.y, spheres[i + 3].m_center.y);
		const __m128 centerZ = _mm_setr_ps(spheres[i].m_center.z, spheres[i + 1].m_center.z, spheres[i + 2].m_center.z, spheres[i + 3].m_center.z);
		const __m128 negativeRadius = _mm_setr_ps(-spheres[i].m_radius, -spheres[i + 1].m_radius, -spheres[i + 2].m_radius, -spheres[i + 3].m_radius);
		__m128 rejected = _mm_setzero_ps();
		for (uint32_t p = 0; p < 6; ++p)
		{
			__m128 distance = _mm_mul_ps(_mm_set1_ps(m_planes[p].m_abcd.x), centerX);
			distance = _mm_add_ps(distance, _mm_mul_ps(_mm_set1_ps(m_planes[p].m_abcd.y), centerY));
			distance = _mm_add_ps(distance, _mm_mul_ps(_mm_set1_ps(m_planes[p].m_abcd.z), centerZ));
			distance = _mm_add_ps(distance, _mm_set1_ps(m_planes[p].m_abcd.w));
			rejected = _mm_or_ps(rejected, _mm_cmplt_ps(distance, negativeRadius));
		}
		const __m128i results = _mm_and_si128(_mm_castps_si128(rejected), _mm_set1_epi32(1));
		_mm_storeu_si128(reinterpret_cast<__m128i*>(outResults + i), results);
	}
#endif
	for (; i < numObjects; ++i)
	{
		outResults[i] = OverlapsSphere(spheres[i]) ? 0 : 1;
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
	const glm::vec3 center = transformMatrix * glm::vec4(GetCenter(), 1.0f);
	const glm::vec3 extents = GetExtents();
	const glm::vec3 transformedExtents =
		glm::abs(glm::vec3(transformMatrix[0])) * extents.x +
		glm::abs(glm::vec3(transformMatrix[1])) * extents.y +
		glm::abs(glm::vec3(transformMatrix[2])) * extents.z;
	m_min = center - transformedExtents;
	m_max = center + transformedExtents;
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

	if (Math::IntersectRayTriangle(ray.GetOrigin(), ray.GetDirection(), tri.m_vertices[0], tri.m_vertices[1], tri.m_vertices[2], baryPosition, distance))
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
	const glm::vec3 t1 = (bmin - ray.GetOrigin()) * ray.GetReciprocalDirection();
	const glm::vec3 t2 = (bmax - ray.GetOrigin()) * ray.GetReciprocalDirection();

	const glm::vec3 tmin3 = glm::min(t1, t2);
	const glm::vec3 tmax3 = glm::max(t1, t2);

	const float tmin = glm::max(tmin3.x, glm::max(tmin3.y, tmin3.z));
	const float tmax = glm::min(tmax3.x, glm::min(tmax3.y, tmax3.z));

	if (tmax >= tmin && tmin < maxRayLength && tmax > 0.0f)
	{
		return tmin;
	}

	return std::numeric_limits<float>::max();
}
