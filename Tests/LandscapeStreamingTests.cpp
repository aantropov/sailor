#include "AssetRegistry/Landscape/LandscapeVegetationAsset.h"
#include "ECS/LandscapeStreaming.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

using namespace Sailor;

namespace
{
	class ScopedTemporaryDirectory final
	{
	public:
		ScopedTemporaryDirectory()
		{
			const auto suffix = std::chrono::steady_clock::now()
				.time_since_epoch()
				.count();
			m_path = std::filesystem::temp_directory_path() /
				("sailor-landscape-vegetation-" + std::to_string(suffix));
			std::filesystem::create_directories(m_path);
		}

		~ScopedTemporaryDirectory()
		{
			std::error_code error;
			std::filesystem::remove_all(m_path, error);
		}

		const std::filesystem::path& GetPath() const { return m_path; }

	private:
		std::filesystem::path m_path{};
	};

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

	void TestVegetationBinaryRoundTripsMatricesAndInstanceSettings()
	{
		ScopedTemporaryDirectory directory;
		const auto filepath = directory.GetPath() / "Landscape.vegetation";

		LandscapeVegetationAssetData source;
		source.m_chunksX = 2u;
		source.m_chunksZ = 1u;
		source.m_profileCount = 2u;
		source.m_chunkSize = 64.0f;
		source.m_chunks.Resize(2u);
		source.m_chunks[0].m_chunkX = 0u;
		source.m_chunks[0].m_chunkZ = 0u;
		source.m_chunks[1].m_chunkX = 1u;
		source.m_chunks[1].m_chunkZ = 0u;

		LandscapeVegetationInstance first;
		first.m_stableId = 0x1122334455667788ull;
		first.m_profileIndex = 1u;
		first.m_lodBias = -2;
		first.m_cullDistanceScale = 0.75f;
		first.m_shadowDistanceScale = 1.5f;
		first.m_transform = glm::mat4(1.0f);
		first.m_transform[0][0] = 1.25f;
		first.m_transform[1][1] = 2.0f;
		first.m_transform[2][2] = 0.75f;
		first.m_transform[3] = glm::vec4(-13.0f, 4.0f, 15.0f, 1.0f);
		source.m_chunks[0].m_instances.Add(first);

		LandscapeVegetationInstance second;
		second.m_stableId = 0x8877665544332211ull;
		second.m_profileIndex = 0u;
		second.m_flags = 0u;
		second.m_transform[3] = glm::vec4(12.0f, 3.0f, -7.0f, 1.0f);
		second.m_lodBias = 3;
		second.m_cullDistanceScale = 2.0f;
		second.m_shadowDistanceScale = 0.5f;
		source.m_chunks[1].m_instances.Add(second);

		std::string diagnostic;
		Require(source.Save(filepath, diagnostic), diagnostic);
		Require(std::filesystem::file_size(filepath) ==
			LandscapeVegetationHeaderSize +
			2u * LandscapeVegetationChunkRecordSize +
			2u * LandscapeVegetationInstanceRecordSize,
			"the vegetation writer must use the documented fixed record layout");

		LandscapeVegetationAssetData loaded;
		Require(loaded.Load(filepath, diagnostic), diagnostic);
		Require(loaded.m_chunksX == source.m_chunksX &&
			loaded.m_chunksZ == source.m_chunksZ &&
			loaded.m_profileCount == source.m_profileCount &&
			loaded.m_chunkSize == source.m_chunkSize &&
			loaded.GetInstanceCount() == 2u,
			"the vegetation header must round-trip exactly");
		Require(loaded.m_chunks[0].m_enabledInstancesPerProfile.Num() == 2u &&
			loaded.m_chunks[0].m_enabledInstancesPerProfile[1] == 1u &&
			loaded.m_chunks[1].m_enabledInstancesPerProfile[0] == 0u,
			"the runtime profile-count index must include only enabled instances");
		const auto& loadedFirst = loaded.m_chunks[0].m_instances[0];
		Require(loadedFirst.m_stableId == first.m_stableId &&
			loadedFirst.m_profileIndex == first.m_profileIndex &&
			loadedFirst.m_flags == first.m_flags &&
			loadedFirst.m_lodBias == first.m_lodBias &&
			loadedFirst.m_cullDistanceScale == first.m_cullDistanceScale &&
			loadedFirst.m_shadowDistanceScale == first.m_shadowDistanceScale,
			"per-instance identity and render settings must round-trip exactly");
		for (glm::length_t column = 0; column < 4; ++column)
		{
			for (glm::length_t row = 0; row < 4; ++row)
			{
				Require(loadedFirst.m_transform[column][row] ==
					first.m_transform[column][row],
					"the column-major instance matrix must round-trip exactly");
			}
		}
		Require(!loaded.m_chunks[1].m_instances[0].IsEnabled(),
			"disabled authored instances must remain in the asset");

		std::fstream corrupt(filepath, std::ios::binary | std::ios::in | std::ios::out);
		Require(corrupt.is_open(), "the test must be able to corrupt the saved file");
		corrupt.seekp(0);
		corrupt.put('X');
		corrupt.close();
		LandscapeVegetationAssetData rejected;
		Require(!rejected.Load(filepath, diagnostic),
			"a vegetation asset with invalid magic must be rejected");
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
		{ "VegetationBinaryRoundTripsMatricesAndInstanceSettings", TestVegetationBinaryRoundTripsMatricesAndInstanceSettings },
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
