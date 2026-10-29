#pragma once

#include "Containers/Octree.h"
#include "Memory/UniquePtr.hpp"
#include "RHI/Reflections.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace Sailor
{
	inline float CalculateReflectionProbeWorldRadius(float radius, const glm::mat4& worldMatrix)
	{
		const float scale = (std::max)({ glm::length(glm::vec3(worldMatrix[0])),
			glm::length(glm::vec3(worldMatrix[1])), glm::length(glm::vec3(worldMatrix[2])) });
		return (std::max)(0.01f, radius * scale);
	}

	struct ReflectionProbeSpatialEntry
	{
		glm::vec3 m_center{};
		float m_radius = 1.0f;
		uint32_t m_probeIndex = RHI::InvalidReflectionProbeIndex;
		std::string m_ownerId;

		bool operator==(const ReflectionProbeSpatialEntry&) const = default;
	};

	// Published once on the world thread and retained by mesh preparation workers.
	class ReflectionProbeSpatialSnapshot final
	{
	public:
		ReflectionProbeSpatialSnapshot(uint64_t revision, TVector<ReflectionProbeSpatialEntry> entries) :
			m_revision(revision), m_entries(std::move(entries))
		{
			float extent = 4.0f;
			for (const auto& entry : m_entries)
			{
				const auto outer = glm::abs(entry.m_center) + glm::vec3(entry.m_radius + 2.0f);
				extent = (std::max)({ extent, outer.x, outer.y, outer.z });
			}
			uint32_t size = 16u;
			while (static_cast<double>(size) * 0.5 <= extent && size < (1u << 30u))
			{
				size <<= 1u;
			}
			m_octree = TUniquePtr<TOctree<uint32_t>>::Make(glm::ivec3(0), size, 4u);
			for (uint32_t index = 0u; index < m_entries.Num(); ++index)
			{
				const auto& entry = m_entries[index];
				const glm::ivec3 center(glm::floor(entry.m_center));
				const glm::ivec3 extents(glm::ceil(glm::abs(entry.m_center - glm::vec3(center)) +
					glm::vec3(entry.m_radius)));
				const bool inserted = m_octree->Insert(center, extents, index);
				check(inserted);
				(void)inserted;
			}
		}

		uint64_t GetRevision() const { return m_revision; }
		const TVector<ReflectionProbeSpatialEntry>& GetEntries() const { return m_entries; }

		uint32_t FindNearest(const glm::vec3& point) const
		{
			uint32_t nearest = RHI::InvalidReflectionProbeIndex;
			float nearestDistance = (std::numeric_limits<float>::max)();
			const ReflectionProbeSpatialEntry* selected = nullptr;
			m_octree->TracePoint(point, [&](uint32_t entryIndex)
			{
				const auto& entry = m_entries[entryIndex];
				const glm::vec3 delta = point - entry.m_center;
				const float distance = glm::dot(delta, delta);
				if (distance <= entry.m_radius * entry.m_radius &&
					(distance < nearestDistance || (distance == nearestDistance &&
						(!selected || entry.m_ownerId < selected->m_ownerId ||
							(entry.m_ownerId == selected->m_ownerId && entry.m_probeIndex < nearest)))))
				{
					selected = &entry;
					nearestDistance = distance;
					nearest = entry.m_probeIndex;
				}
			});
			return nearest;
		}

	private:
		uint64_t m_revision = 0u;
		TVector<ReflectionProbeSpatialEntry> m_entries;
		TUniquePtr<TOctree<uint32_t>> m_octree;
	};
}
