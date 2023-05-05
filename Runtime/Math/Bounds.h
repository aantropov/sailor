#pragma once
#include <glm/glm/glm.hpp>
#include <glm/glm/gtx/hash.hpp>
#include "Memory/Memory.h"
#include "Memory/LockFreeHeapAllocator.h"
#include "Containers/Vector.h"
#include "Containers/Hash.h"
#include "Math/Transform.h"

namespace Sailor::Math
{
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
		glm::vec3 m_center;
		float m_radius;

		Sphere() : m_center(0.0f, 0.0f, 0.0f), m_radius(1.0f) {}
		Sphere(glm::vec3 center, float radius) : m_center(center), m_radius(radius) {}
	};

	struct AABB
	{
		glm::vec3 m_min{};
		glm::vec3 m_max{};

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
