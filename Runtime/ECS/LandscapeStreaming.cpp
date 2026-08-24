#include "ECS/LandscapeStreaming.h"

#include <algorithm>
#include <cmath>

using namespace Sailor;

TVector<uint32_t> Sailor::BuildLandscapeLodCoordinates(
	uint32_t resolution,
	uint32_t stride)
{
	TVector<uint32_t> result;
	if (resolution == 0u)
	{
		return result;
	}

	stride = (std::max)(stride, 1u);
	result.Reserve(static_cast<size_t>(resolution / stride) + 2u);
	for (uint32_t coordinate = 0u; coordinate < resolution;)
	{
		result.Add(coordinate);
		const uint32_t remaining = resolution - coordinate;
		coordinate += (std::min)(stride, remaining);
	}
	result.Add(resolution);
	return result;
}

void Sailor::AppendLandscapeLodIndices(
	uint32_t resolution,
	const TVector<uint32_t>& coordinates,
	bool bIncludeSkirts,
	TVector<uint32_t>& indices)
{
	if (resolution == 0u || coordinates.Num() < 2u)
	{
		return;
	}

	const uint32_t row = resolution + 1u;
	for (size_t z = 0u; z + 1u < coordinates.Num(); ++z)
	{
		for (size_t x = 0u; x + 1u < coordinates.Num(); ++x)
		{
			const uint32_t i0 = coordinates[z] * row + coordinates[x];
			const uint32_t i1 = coordinates[z] * row + coordinates[x + 1u];
			const uint32_t i2 = coordinates[z + 1u] * row + coordinates[x];
			const uint32_t i3 = coordinates[z + 1u] * row + coordinates[x + 1u];
			indices.AddRange({ i0, i2, i1, i1, i2, i3 });
		}
	}
	if (!bIncludeSkirts)
	{
		return;
	}

	const uint32_t baseVertexCount = row * row;
	auto appendEdge = [&indices, &coordinates](
		auto topIndex,
		auto skirtIndex,
		bool bReverseWinding)
		{
			for (size_t segment = 0u; segment + 1u < coordinates.Num(); ++segment)
			{
				const uint32_t coordinate0 = coordinates[segment];
				const uint32_t coordinate1 = coordinates[segment + 1u];
				const uint32_t top0 = topIndex(coordinate0);
				const uint32_t top1 = topIndex(coordinate1);
				const uint32_t bottom0 = skirtIndex(coordinate0);
				const uint32_t bottom1 = skirtIndex(coordinate1);
				if (bReverseWinding)
				{
					indices.AddRange({ top0, bottom0, top1, top1, bottom0, bottom1 });
				}
				else
				{
					indices.AddRange({ top0, top1, bottom0, top1, bottom1, bottom0 });
				}
			}
		};

	appendEdge(
		[](uint32_t x) { return x; },
		[baseVertexCount](uint32_t x) { return baseVertexCount + x; },
		false);
	appendEdge(
		[row, resolution](uint32_t x) { return resolution * row + x; },
		[baseVertexCount, row](uint32_t x) { return baseVertexCount + row + x; },
		true);
	appendEdge(
		[row](uint32_t z) { return z * row; },
		[baseVertexCount, row](uint32_t z) { return baseVertexCount + row * 2u + z; },
		true);
	appendEdge(
		[row, resolution](uint32_t z) { return z * row + resolution; },
		[baseVertexCount, row](uint32_t z) { return baseVertexCount + row * 3u + z; },
		false);
}

void Sailor::SelectLandscapeGrassResidency(
	TVector<LandscapeGrassCandidate>& candidates,
	uint32_t instanceBudget,
	TVector<LandscapeGrassSelection>& outSelections)
{
	candidates.Sort([](
		const LandscapeGrassCandidate& lhs,
		const LandscapeGrassCandidate& rhs)
		{
			if (lhs.m_chunkRing != rhs.m_chunkRing)
			{
				return lhs.m_chunkRing < rhs.m_chunkRing;
			}
			if (lhs.m_bChunkResident != rhs.m_bChunkResident)
			{
				return lhs.m_bChunkResident;
			}
			if (lhs.m_chunkManhattanDistance != rhs.m_chunkManhattanDistance)
			{
				return lhs.m_chunkManhattanDistance < rhs.m_chunkManhattanDistance;
			}
			if (lhs.m_componentIndex != rhs.m_componentIndex)
			{
				return lhs.m_componentIndex < rhs.m_componentIndex;
			}
			if (lhs.m_chunkIndex != rhs.m_chunkIndex)
			{
				return lhs.m_chunkIndex < rhs.m_chunkIndex;
			}
			const float lhsPriority = std::isfinite(lhs.m_priority) ? lhs.m_priority : 0.0f;
			const float rhsPriority = std::isfinite(rhs.m_priority) ? rhs.m_priority : 0.0f;
			if (lhsPriority != rhsPriority)
			{
				return lhsPriority > rhsPriority;
			}
			return lhs.m_profileIndex < rhs.m_profileIndex;
		});

	outSelections.Clear(false);
	for (const auto& candidate : candidates)
	{
		if (instanceBudget == 0u)
		{
			break;
		}
		const uint32_t selectedCount = (std::min)(
			candidate.m_capacity,
			instanceBudget);
		if (selectedCount == 0u)
		{
			continue;
		}
		LandscapeGrassSelection selection;
		selection.m_componentIndex = candidate.m_componentIndex;
		selection.m_chunkIndex = candidate.m_chunkIndex;
		selection.m_profileIndex = candidate.m_profileIndex;
		selection.m_instanceCount = selectedCount;
		outSelections.Add(std::move(selection));
		instanceBudget -= selectedCount;
	}
}

bool Sailor::DoesLandscapeGrassChunkOverlapFrustum(
	const Math::AABB& worldBounds,
	const Math::Frustum& frustum,
	float residencyMargin)
{
	if (!worldBounds.IsValid())
	{
		return false;
	}

	Math::AABB cullingBounds = worldBounds;
	const float margin = std::isfinite(residencyMargin) ?
		(std::max)(residencyMargin, 0.0f) : 0.0f;
	cullingBounds.m_min -= glm::vec3(margin);
	cullingBounds.m_max += glm::vec3(margin);
	return frustum.OverlapsAABB(cullingBounds);
}
