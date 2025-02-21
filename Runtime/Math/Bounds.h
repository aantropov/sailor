#pragma once
#include "Memory/Memory.h"
#include "Memory/LockFreeHeapAllocator.h"
#include "Containers/Vector.h"
#include "Containers/Hash.h"
#include "Math/Transform.h"

namespace Sailor::Math
{
	struct Triangle
	{
		vec3 m_centroid{};
		vec3 m_vertices[3];
		vec3 m_normals[3];
		vec3 m_tangent[3];
		vec3 m_bitangent[3];
		vec2 m_uvs[3];
		vec2 m_uvs2[3];
		u8 m_materialIndex{};

		float SquareArea() const;
		float Area() const;
	};

	struct Ray
	{
		Ray() { O4 = D4 = rD4 = _mm_set1_ps(1); }
		Ray(const vec3& origin, const vec3& direction) : m_origin(origin)
		{
			SetDirection(direction);
		}

		__forceinline const vec3& GetOrigin() const { return m_origin; }
		__forceinline const vec3& GetDirection() const { return m_direction; }
		__forceinline const vec3& GetReciprocalDirection() const { return m_rDirection; }

		__forceinline const __m128& GetOrigin4() const { return O4; }
		__forceinline const __m128& GetDirection4() const { return D4; }
		__forceinline const __m128& GetReciprocalDirection4() const { return rD4; }

		__forceinline void SetOrigin(const vec3& value) { m_origin = value; }
		__forceinline void SetDirection(const vec3& value)
		{
			m_direction = value;
			m_rDirection = 1.0f / m_direction;
		}

	protected:

		union { struct { vec3 m_origin; float dummy1; }; __m128 O4; };
		union { struct { vec3 m_direction; float dummy2; }; __m128 D4; };
		union { struct { vec3 m_rDirection; float dummy3; }; __m128 rD4; };

		friend float IntersectRayAABB(const Ray& ray, const __m128 bmin4, const __m128 bmax4, float maxRayLength);
	};

	struct RaycastHit
	{
		static constexpr float NoIntersection = std::numeric_limits<float>().infinity();

		bool HasIntersection() const { return m_point != vec3(NoIntersection); }
		vec3 m_point = vec3(NoIntersection);
		vec3 m_normal{};
		vec2 m_textureCoord{};
		vec2 m_textureCoord2{};
		vec3 m_barycentricCoordinate{};
		float m_rayLenght = NoIntersection;
		uint32_t m_triangleIndex = (uint32_t)-1;
	};

	struct Plane
	{
		glm::vec4 m_abcd = { 0.f, 1.f, 0.f, 0.0f };

		Plane() : m_abcd(0.0f, 0.0f, 0.0f, 0.0f) {}
		Plane(glm::vec4 abcd) : m_abcd(abcd) {}
		Plane(glm::vec3 normal, float distance) : m_abcd(normal.x, normal.y, normal.z, distance) {}
		Plane(glm::vec3 normal, glm::vec3 point)
		{
			m_abcd.x = normal.x;
			m_abcd.y = normal.y;
			m_abcd.z = normal.z;

			m_abcd.w = -glm::dot(glm::vec3(point), normal);
		}

		float& operator[] (uint32_t i) { return m_abcd[i]; }
		float operator[] (uint32_t i) const { return m_abcd[i]; }

		glm::vec3 GetNormal() const { return glm::vec3(m_abcd.x, m_abcd.y, m_abcd.z); }

		void Normalize();
	};

	struct Sphere
	{
		union
		{
			struct { glm::vec3 m_center; float m_radius; };
			glm::vec4 m_vec4;
		};

		const vec4& GetVec4() const { return m_vec4; }
		Sphere() : m_center(0.0f, 0.0f, 0.0f), m_radius(1.0f) {}
		Sphere(glm::vec3 center, float radius) : m_center(center), m_radius(radius) {}
	};

	struct AABB
	{
		glm::vec3 m_min = vec3(10e30f);
		glm::vec3 m_max = -vec3(10e30f);

		AABB() = default;
		AABB(glm::vec3 center, glm::vec3 extents);

		template<typename TContainer>
		SAILOR_API __forceinline void GetPoints(TContainer& outPoints) const
		{
			outPoints.AddRange({
			glm::vec3(m_min.x, m_min.y, m_min.z),
			glm::vec3(m_max.x, m_max.y, m_max.z),
			glm::vec3(m_min.x, m_max.y, m_max.z),
			glm::vec3(m_max.x, m_min.y, m_max.z),
			glm::vec3(m_max.x, m_max.y, m_min.z),
			glm::vec3(m_max.x, m_min.y, m_min.z),
			glm::vec3(m_min.x, m_max.y, m_min.z),
			glm::vec3(m_min.x, m_min.y, m_max.z) });
		}

		SAILOR_API __forceinline glm::vec3 GetCenter() const;
		SAILOR_API __forceinline glm::vec3 GetExtents() const;

		SAILOR_API __forceinline Sphere ToSphere() const;

		SAILOR_API __forceinline float Volume() const;
		SAILOR_API __forceinline float Area() const;
		SAILOR_API __forceinline void Extend(const AABB& inner);
		SAILOR_API __forceinline void Extend(const glm::vec3& inner);
		SAILOR_API __forceinline void Apply(const glm::mat4& transformMatrix);
		SAILOR_API __forceinline bool IsValid() const { return m_min != m_max; }

		SAILOR_API bool operator==(const AABB& rhs) const { return this->m_max == rhs.m_max && this->m_min == rhs.m_min; }
	};

	struct Frustum
	{
		SAILOR_API Frustum() : m_corners(8) {}
		SAILOR_API Frustum(const glm::mat4& projectionViewMatrix) : m_corners(8) { ExtractFrustumPlanes(projectionViewMatrix); }

		SAILOR_API __forceinline bool OverlapsAABB(const AABB& aabb) const;
		SAILOR_API __forceinline bool OverlapsSphere(const Sphere& sphere) const;

		// SSE version, the most optimized
		SAILOR_API __forceinline void OverlapsAABB(AABB* aabb, uint32_t numObjects, int32_t* outResults) const;

		// SSE version, the most optimized
		SAILOR_API __forceinline void OverlapsSphere(Sphere* spheres, uint32_t numObjects, int32_t* outResults) const;

		SAILOR_API __forceinline void ContainsSphere(Sphere* spheres, uint32_t numObjects, int32_t* outResults) const;
		SAILOR_API __forceinline bool ContainsPoint(const glm::vec3& point) const;
		SAILOR_API __forceinline bool ContainsSphere(const Sphere& sphere) const;

		SAILOR_API __forceinline glm::vec3 CalculateCenter() const;
		SAILOR_API __forceinline glm::mat4 CalculateOrthoMatrixByView(const glm::mat4& view, float zMult) const;

		SAILOR_API __forceinline const TVector<glm::vec3>& GetCorners() const;

		SAILOR_API __forceinline void ExtractFrustumPlanes(const glm::mat4& projectionViewMatrix, bool bNormalizePlanes = true);
		SAILOR_API __forceinline void ExtractFrustumPlanes(const glm::mat4& worldMatrix, float aspect, float fovY, float zNear, float zFar);

	protected:

		void CalculateCorners(const glm::mat4& matrix, bool bReverseZ);

		// Normals are directed inside frustum
		// The order is: Left, Right, Top, Bottom, Near, Far
		Plane m_planes[6];

		// The order is: 
		//  Far:  Near:
		//  1  0  5  4
		//  2  3  6  7
		TVector<glm::vec3> m_corners;
	};

	bool IntersectRayTriangle(const Ray& ray, const Triangle& tri, RaycastHit& outRaycastHit, float maxRayLength = FLT_MAX);
	bool IntersectRayTriangle(const Ray& ray, const TVector<Triangle>& tris, RaycastHit& outRaycastHit, float maxRayLength = FLT_MAX);
	bool IntersectRayTriangle(const glm::vec3& r0, const glm::vec3& rd, const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, glm::vec3& outBarycentric, float& outDistance);

	// Copied from GLM version, with changed epsilon to zero
	template<typename T, qualifier Q>
	bool IntersectRayTriangle
	(
		vec<3, T, Q> const& orig, vec<3, T, Q> const& dir,
		vec<3, T, Q> const& vert0, vec<3, T, Q> const& vert1, vec<3, T, Q> const& vert2,
		vec<2, T, Q>& baryPosition, T& distance
	)
	{
		// find vectors for two edges sharing vert0
		vec<3, T, Q> const edge1 = vert1 - vert0;
		vec<3, T, Q> const edge2 = vert2 - vert0;

		// begin calculating determinant - also used to calculate U parameter
		vec<3, T, Q> const p = glm::cross(dir, edge2);

		// if determinant is near zero, ray lies in plane of triangle
		T const det = glm::dot(edge1, p);

		vec<3, T, Q> Perpendicular(0);

		if (det > 0.0f)
		{
			// calculate distance from vert0 to ray origin
			vec<3, T, Q> const dist = orig - vert0;

			// calculate U parameter and test bounds
			baryPosition.x = glm::dot(dist, p);
			if (baryPosition.x < static_cast<T>(0) || baryPosition.x > det)
				return false;

			// prepare to test V parameter
			Perpendicular = glm::cross(dist, edge1);

			// calculate V parameter and test bounds
			baryPosition.y = glm::dot(dir, Perpendicular);
			if ((baryPosition.y < static_cast<T>(0)) || ((baryPosition.x + baryPosition.y) > det))
				return false;
		}
		else if (det < 0.0f)
		{
			// calculate distance from vert0 to ray origin
			vec<3, T, Q> const dist = orig - vert0;

			// calculate U parameter and test bounds
			baryPosition.x = glm::dot(dist, p);
			if ((baryPosition.x > static_cast<T>(0)) || (baryPosition.x < det))
				return false;

			// prepare to test V parameter
			Perpendicular = glm::cross(dist, edge1);

			// calculate V parameter and test bounds
			baryPosition.y = glm::dot(dir, Perpendicular);
			if ((baryPosition.y > static_cast<T>(0)) || (baryPosition.x + baryPosition.y < det))
				return false;
		}
		else
			return false; // ray is parallel to the plane of the triangle

		T inv_det = static_cast<T>(1) / det;

		// calculate distance, ray intersects triangle
		distance = glm::dot(edge2, Perpendicular) * inv_det;
		baryPosition *= inv_det;

		return true;
	}

	// Return value is distance to AABB
	float IntersectRayAABB(const Ray& ray, const glm::vec3& bmin, const glm::vec3& bmax, float maxRayLength = FLT_MAX);
}

namespace std
{
	template<>
	struct std::hash<Sailor::Math::AABB>
	{
		SAILOR_API size_t operator()(Sailor::Math::AABB const& instance) const
		{
			static std::hash<glm::vec3> p;

			size_t h = 0;
			Sailor::HashCombine(h, p(instance.m_min), p(instance.m_max));

			return h;
		}
	};
}
