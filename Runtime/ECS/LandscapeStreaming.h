#pragma once

#include "Core/Defines.h"
#include "Memory/LockFreeHeapAllocator.h"
#include "Containers/Vector.h"
#include "Math/Bounds.h"

namespace Sailor
{
	struct LandscapeGrassCandidate final
	{
		size_t m_componentIndex = 0u;
		size_t m_chunkIndex = 0u;
		size_t m_profileIndex = 0u;
		uint32_t m_capacity = 0u;
		uint32_t m_chunkRing = 0u;
		uint32_t m_chunkManhattanDistance = 0u;
		float m_priority = 1.0f;
		bool m_bChunkResident = false;
	};

	struct LandscapeGrassSelection final
	{
		size_t m_componentIndex = 0u;
		size_t m_chunkIndex = 0u;
		size_t m_profileIndex = 0u;
		uint32_t m_instanceCount = 0u;
		uint64_t m_viewRevision = 0u;
	};

	SAILOR_API TVector<uint32_t> BuildLandscapeLodCoordinates(
		uint32_t resolution,
		uint32_t stride);
	SAILOR_API void AppendLandscapeLodIndices(
		uint32_t resolution,
		const TVector<uint32_t>& coordinates,
		bool bIncludeSkirts,
		TVector<uint32_t>& indices);

	SAILOR_API void SelectLandscapeGrassResidency(
		TVector<LandscapeGrassCandidate>& candidates,
		uint32_t instanceBudget,
		TVector<LandscapeGrassSelection>& outSelections);
	SAILOR_API bool DoesLandscapeGrassChunkOverlapFrustum(
		const Math::AABB& worldBounds,
		const Math::Frustum& frustum,
		float residencyMargin = 0.0f);
}
