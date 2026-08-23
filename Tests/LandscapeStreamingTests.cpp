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

	void TestGrassSelectionFillsStableChunkRingsWithinGlobalBudget()
	{
		TVector<LandscapeGrassCandidate> candidates = {
			{ 0u, 7u, 1u, 4u, 1u, 1u, 100.0f, false },
			{ 0u, 3u, 0u, 3u, 0u, 0u, 1.0f, false },
			{ 0u, 3u, 1u, 2u, 0u, 0u, 2.0f, false },
			{ 0u, 9u, 0u, 5u, 1u, 2u, 1.0f, true },
		};

		TVector<LandscapeGrassSelection> first;
		TVector<LandscapeGrassSelection> second;
		SelectLandscapeGrassResidency(candidates, 8u, first);
		SelectLandscapeGrassResidency(candidates, 8u, second);
		Require(first.Num() == 3u && second.Num() == first.Num(),
			"the hard budget must select exactly three candidate ranges");
		Require(first[0].m_chunkIndex == 3u && first[0].m_profileIndex == 1u &&
			first[0].m_instanceCount == 2u,
			"the current chunk must be filled first and use profile priority within that chunk");
		Require(first[1].m_chunkIndex == 3u && first[1].m_profileIndex == 0u &&
			first[1].m_instanceCount == 3u,
			"all admitted profiles from the current chunk must precede neighboring chunks");
		Require(first[2].m_chunkIndex == 9u && first[2].m_instanceCount == 3u,
			"an already resident neighbor must be retained within its ring and may partially fill the budget");

		uint32_t totalInstances = 0u;
		for (size_t index = 0u; index < first.Num(); ++index)
		{
			totalInstances += first[index].m_instanceCount;
			Require(first[index].m_chunkIndex == second[index].m_chunkIndex &&
				first[index].m_profileIndex == second[index].m_profileIndex &&
				first[index].m_instanceCount == second[index].m_instanceCount,
				"identical inputs must produce an identical selection order and count");
		}
		Require(totalInstances == 8u,
			"selected instances must fill the budget without exceeding it");
		TVector<LandscapeGrassSelection> empty;
		SelectLandscapeGrassResidency(candidates, 0u, empty);
		Require(empty.IsEmpty(),
			"a zero instance budget must produce an empty active set");
	}

	void TestGrassResidencyStabilityNeverOverridesANearerRing()
	{
		TVector<LandscapeGrassCandidate> candidates = {
			{ 0u, 0u, 0u, 4u, 1u, 1u, 1.0f, false },
			{ 0u, 1u, 0u, 4u, 1u, 2u, 1.0f, true },
		};
		TVector<LandscapeGrassSelection> retained;
		SelectLandscapeGrassResidency(candidates, 4u, retained);
		Require(retained.Num() == 1u && retained[0].m_chunkIndex == 1u,
			"a resident chunk must remain selected over another candidate in the same ring");

		for (auto& candidate : candidates)
		{
			if (candidate.m_chunkIndex == 0u)
			{
				candidate.m_chunkRing = 0u;
			}
		}
		TVector<LandscapeGrassSelection> replaced;
		SelectLandscapeGrassResidency(candidates, 4u, replaced);
		Require(replaced.Num() == 1u && replaced[0].m_chunkIndex == 0u,
			"a candidate in a nearer chunk ring must replace a resident from a farther ring");
	}

	void TestGrassSelectionUsesOneBudgetAcrossLandscapeComponents()
	{
		TVector<LandscapeGrassCandidate> candidates = {
			{ 1u, 0u, 0u, 4u, 0u, 0u, 1.0f, false },
			{ 0u, 0u, 0u, 4u, 0u, 0u, 1.0f, false },
		};
		TVector<LandscapeGrassSelection> selected;
		SelectLandscapeGrassResidency(candidates, 6u, selected);
		Require(selected.Num() == 2u &&
			selected[0].m_componentIndex == 0u && selected[0].m_instanceCount == 4u &&
			selected[1].m_componentIndex == 1u && selected[1].m_instanceCount == 2u,
			"one deterministic budget must be shared across all landscape components");
	}

	void TestGrassChunksMustOverlapTheCameraFrustum()
	{
		Math::Frustum frustum;
		frustum.ExtractFrustumPlanes(
			glm::mat4(1.0f),
			1.0f,
			90.0f,
			0.1f,
			100.0f);

		Require(DoesLandscapeGrassChunkOverlapFrustum(
			Math::AABB(glm::vec3(0.0f, 0.0f, -8.0f), glm::vec3(1.0f)),
			frustum),
			"a grass chunk in front of the camera must be admitted");
		Require(!DoesLandscapeGrassChunkOverlapFrustum(
			Math::AABB(glm::vec3(0.0f, 0.0f, 8.0f), glm::vec3(1.0f)),
			frustum),
			"a grass chunk behind the camera must not consume the budget");

		const Math::AABB edgeChunk(
			glm::vec3(15.0f, 0.0f, -8.0f),
			glm::vec3(0.5f));
		Require(!DoesLandscapeGrassChunkOverlapFrustum(edgeChunk, frustum),
			"a non-resident chunk outside the side plane must be culled");
		Require(DoesLandscapeGrassChunkOverlapFrustum(edgeChunk, frustum, 8.0f),
			"the residency margin must retain a chunk near a frustum edge");
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "LodCoordinatesPreserveChunkBoundaries", TestLodCoordinatesPreserveChunkBoundaries },
		{ "LodIndicesStayWithinPackedTerrainAndSkirtVertices", TestLodIndicesStayWithinPackedTerrainAndSkirtVertices },
		{ "GrassSelectionFillsStableChunkRingsWithinGlobalBudget", TestGrassSelectionFillsStableChunkRingsWithinGlobalBudget },
		{ "GrassResidencyStabilityNeverOverridesANearerRing", TestGrassResidencyStabilityNeverOverridesANearerRing },
		{ "GrassSelectionUsesOneBudgetAcrossLandscapeComponents", TestGrassSelectionUsesOneBudgetAcrossLandscapeComponents },
		{ "GrassChunksMustOverlapTheCameraFrustum", TestGrassChunksMustOverlapTheCameraFrustum },
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
