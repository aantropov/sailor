#pragma once

#include "Core/Defines.h"
#include <glm/glm/glm.hpp>

namespace Sailor::Math
{
	struct Plane
	{
		glm::vec4 m_abcd = { 0.f, 1.f, 0.f, 0.0f };

		Plane() : m_abcd(0.0f, 0.0f, 0.0f, 0.0f) {}
		Plane(glm::vec4 abcd) : m_abcd(abcd) {}
		Plane(glm::vec3 normal, float distance) : m_abcd(normal.x, normal.y, normal.z, distance) {}

		float& operator[] (uint32_t i) { return m_abcd[i]; }
		float operator[] (uint32_t i) const { return m_abcd[i]; }
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
	};

	class Frustum
	{
		SAILOR_API Frustum() = default;
		SAILOR_API Frustum(const glm::mat4& matrix) { ExtractFrustumPlanes(matrix); }

		// Slow version for individual culling
		SAILOR_API __forceinline bool OverlapsAABB(const AABB& aabb) const;

		// SSE version, the most optimized
		SAILOR_API __forceinline void OverlapsAABB(AABB* aabb, uint32_t numObjects, int32_t* outResults) const;
		SAILOR_API __forceinline bool ContainsPoint(const glm::vec3& point) const;
		SAILOR_API __forceinline bool ContainsSphere(const Sphere& sphere) const;

		SAILOR_API __forceinline void ExtractFrustumPlanes(const glm::mat4& matrix);

	protected:

		// The order is: Left, Right, Top, Bottom, Near, Far
		Plane m_planes[6];
	};
}