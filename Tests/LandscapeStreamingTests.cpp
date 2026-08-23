#include "ECS/LandscapeStreaming.h"

#include <functional>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

using namespace Sailor;

namespace
{
	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	void RequireCoordinates(
		const TVector<uint32_t>& actual,
		std::initializer_list<uint32_t> expected,
		const std::string& message)
	{
		Require(actual.Num() == expected.size(), message + ": unexpected coordinate count");
		size_t index = 0u;
		for (uint32_t coordinate : expected)
		{
			Require(actual[index] == coordinate, message + ": unexpected coordinate value");
			++index;
		}
	}

	void TestLodCoordinatesPreserveChunkBoundaries()
	{
		RequireCoordinates(
			BuildLandscapeLodCoordinates(10u, 4u),
			{ 0u, 4u, 8u, 10u },
			"non-divisible LOD stride must preserve the final chunk edge");
		RequireCoordinates(
			BuildLandscapeLodCoordinates(24u, 4u),
			{ 0u, 4u, 8u, 12u, 16u, 20u, 24u },
			"divisible LOD stride must preserve regular coordinates");
		RequireCoordinates(
			BuildLandscapeLodCoordinates(3u, 0u),
			{ 0u, 1u, 2u, 3u },
			"zero stride must normalize to the full-resolution grid");
		Require(BuildLandscapeLodCoordinates(0u, 4u).IsEmpty(),
			"zero-resolution chunks must not produce LOD coordinates");
	}

	void TestLodIndicesStayWithinPackedTerrainAndSkirtVertices()
	{
		constexpr uint32_t resolution = 10u;
		constexpr uint32_t row = resolution + 1u;
		constexpr uint32_t baseVertexCount = row * row;
		constexpr uint32_t packedVertexCount = baseVertexCount + row * 4u;
		const auto coordinates = BuildLandscapeLodCoordinates(resolution, 4u);

		TVector<uint32_t> terrainIndices;
		AppendLandscapeLodIndices(
			resolution,
			coordinates,
			false,
			terrainIndices);
		Require(terrainIndices.Num() == 54u,
			"a four-coordinate grid must produce nine terrain quads");
		for (uint32_t index : terrainIndices)
		{
			Require(index < baseVertexCount,
				"terrain-only LOD indices must address base vertices");
		}

		TVector<uint32_t> skirtedIndices;
		AppendLandscapeLodIndices(
			resolution,
			coordinates,
			true,
			skirtedIndices);
		Require(skirtedIndices.Num() == 126u,
			"every LOD edge segment must append two skirt triangles");
		bool bReferencesSkirt = false;
		for (uint32_t index : skirtedIndices)
		{
			Require(index < packedVertexCount,
				"skirted LOD indices must remain inside the packed vertex allocation");
			bReferencesSkirt |= index >= baseVertexCount;
		}
		Require(bReferencesSkirt,
			"a skirted LOD range must reference the appended skirt vertices");
	}

	void TestGrassSelectionIsPrioritizedDeterministicAndBudgeted()
	{
		TVector<LandscapeGrassCandidate> candidates = {
			{ 7u, 1u, 5u, 1.0f, 3.0f, false, 0.0f },
			{ 3u, 0u, 4u, 2.0f, 100.0f, false, 0.0f },
			{ 1u, 2u, 5u, 1.0f, 3.0f, true, 0.0f },
			{ 0u, 0u, 5u, 1.0f, 2.0f, false, 0.0f },
		};

		const auto first = SelectLandscapeGrassResidency(candidates, 11u);
		const auto second = SelectLandscapeGrassResidency(candidates, 11u);
		Require(first.Num() == 3u && second.Num() == first.Num(),
			"the hard budget must select exactly three candidate ranges");
		Require(first[0].m_chunkIndex == 3u && first[0].m_instanceCount == 4u,
			"profile priority must win before camera distance");
		Require(first[1].m_chunkIndex == 0u && first[1].m_instanceCount == 5u,
			"camera distance must order candidates with equal priority");
		Require(first[2].m_chunkIndex == 1u && first[2].m_instanceCount == 2u,
			"resident state must stabilize equal candidates and the final range must be partially admitted");

		uint32_t totalInstances = 0u;
		for (size_t index = 0u; index < first.Num(); ++index)
		{
			totalInstances += first[index].m_instanceCount;
			Require(first[index].m_chunkIndex == second[index].m_chunkIndex &&
				first[index].m_profileIndex == second[index].m_profileIndex &&
				first[index].m_instanceCount == second[index].m_instanceCount,
				"identical inputs must produce an identical selection order and count");
		}
		Require(totalInstances == 11u,
			"selected instances must fill the budget without exceeding it");
		Require(SelectLandscapeGrassResidency(candidates, 0u).IsEmpty(),
			"a zero instance budget must produce an empty active set");
	}

	void TestGrassResidencyHysteresisPreventsBoundaryChurn()
	{
		TVector<LandscapeGrassCandidate> candidates = {
			{ 0u, 0u, 4u, 1.0f, 10.0f, false, 0.0f },
			{ 1u, 0u, 4u, 1.0f, 11.0f, true, 2.0f },
		};
		const auto retained = SelectLandscapeGrassResidency(candidates, 4u);
		Require(retained.Num() == 1u && retained[0].m_chunkIndex == 1u,
			"a resident chunk must remain selected while the challenger is inside its hysteresis margin");

		candidates[1].m_distance = 13.0f;
		const auto replaced = SelectLandscapeGrassResidency(candidates, 4u);
		Require(replaced.Num() == 1u && replaced[0].m_chunkIndex == 0u,
			"a meaningfully nearer chunk must replace a resident beyond its hysteresis margin");
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "LodCoordinatesPreserveChunkBoundaries", TestLodCoordinatesPreserveChunkBoundaries },
		{ "LodIndicesStayWithinPackedTerrainAndSkirtVertices", TestLodIndicesStayWithinPackedTerrainAndSkirtVertices },
		{ "GrassSelectionIsPrioritizedDeterministicAndBudgeted", TestGrassSelectionIsPrioritizedDeterministicAndBudgeted },
		{ "GrassResidencyHysteresisPreventsBoundaryChurn", TestGrassResidencyHysteresisPreventsBoundaryChurn },
	};

	for (const auto& test : tests)
	{
		try
		{
			test.second();
			std::cout << "[PASS] " << test.first << std::endl;
		}
		catch (const std::exception& error)
		{
			std::cerr << "[FAIL] " << test.first << ": " << error.what() << std::endl;
			return 1;
		}
	}

	return 0;
}
