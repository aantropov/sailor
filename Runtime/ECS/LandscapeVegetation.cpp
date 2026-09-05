#include "ECS/LandscapeECSInternal.h"

#include "AssetRegistry/AssetRegistry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace Sailor::LandscapeECSInternal
{
	static void MarkChunksIntersectingStamp(LandscapeData& data,
		const TVector<float>& stamps,
		size_t offset,
		float margin)
	{
		if (offset + 4u >= stamps.Num())
		{
			return;
		}

		const float radius = (std::max)(stamps[offset + 2u], 0.001f) + margin;
		const glm::vec2 center(stamps[offset], stamps[offset + 1u]);
		const float landscapeWidth = data.m_chunksX * data.m_chunkSize;
		const float landscapeDepth = data.m_chunksZ * data.m_chunkSize;
		for (uint32_t z = 0u; z < data.m_chunksZ; ++z)
		{
			for (uint32_t x = 0u; x < data.m_chunksX; ++x)
			{
				const glm::vec2 minimum(
					x * data.m_chunkSize - landscapeWidth * 0.5f, z * data.m_chunkSize - landscapeDepth * 0.5f);
				const glm::vec2 maximum = minimum + glm::vec2(data.m_chunkSize);
				const glm::vec2 closest = glm::clamp(center, minimum, maximum);
				if (glm::distance(center, closest) <= radius)
				{
					data.m_dirtyChunks.Insert(z * data.m_chunksX + x);
				}
			}
		}
	}

	void MarkChunksAffectedByStampChanges(LandscapeData& data,
		const TVector<float>& previous,
		const TVector<float>& current,
		float margin)
	{
		const size_t numStamps = (std::max)(previous.Num(), current.Num()) / 5u;
		for (size_t stampIndex = 0u; stampIndex < numStamps; ++stampIndex)
		{
			const size_t offset = stampIndex * 5u;
			bool bSame = offset + 4u < previous.Num() && offset + 4u < current.Num();
			for (size_t valueIndex = 0u; bSame && valueIndex < 5u; ++valueIndex)
			{
				bSame = previous[offset + valueIndex] == current[offset + valueIndex];
			}
			if (bSame)
			{
				continue;
			}

			MarkChunksIntersectingStamp(data, previous, offset, margin);
			MarkChunksIntersectingStamp(data, current, offset, margin);
		}
	}

	static float SampleLandscapeChunkHeight(const LandscapeData& data, const LandscapeChunk& chunk, float x, float z)
	{
		const uint32_t resolution = chunk.m_heightResolution;
		const uint32_t row = resolution + 1u;
		if (resolution == 0u || chunk.m_heightSamples.Num() < static_cast<size_t>(row) * row)
		{
			return 0.0f;
		}

		const float landscapeWidth = data.m_chunksX * data.m_chunkSize;
		const float landscapeDepth = data.m_chunksZ * data.m_chunkSize;
		const float originX = chunk.m_chunkX * data.m_chunkSize - landscapeWidth * 0.5f;
		const float originZ = chunk.m_chunkZ * data.m_chunkSize - landscapeDepth * 0.5f;
		const float sampleX = (std::clamp)((x - originX) / data.m_chunkSize, 0.0f, 1.0f) * resolution;
		const float sampleZ = (std::clamp)((z - originZ) / data.m_chunkSize, 0.0f, 1.0f) * resolution;
		const uint32_t x0 = static_cast<uint32_t>(std::floor(sampleX));
		const uint32_t z0 = static_cast<uint32_t>(std::floor(sampleZ));
		const uint32_t x1 = (std::min)(x0 + 1u, resolution);
		const uint32_t z1 = (std::min)(z0 + 1u, resolution);
		const float tx = sampleX - static_cast<float>(x0);
		const float tz = sampleZ - static_cast<float>(z0);
		const float a = glm::mix(chunk.m_heightSamples[static_cast<size_t>(z0) * row + x0],
			chunk.m_heightSamples[static_cast<size_t>(z0) * row + x1],
			tx);
		const float b = glm::mix(chunk.m_heightSamples[static_cast<size_t>(z1) * row + x0],
			chunk.m_heightSamples[static_cast<size_t>(z1) * row + x1],
			tx);
		return glm::mix(a, b, tz);
	}

	bool TryLoadVegetationAsset(LandscapeData& data)
	{
		if (!data.m_bReloadVegetationAsset)
		{
			return data.m_bVegetationAssetLoaded;
		}
		data.m_bReloadVegetationAsset = false;
		if (!data.m_vegetationAsset)
		{
			data.m_vegetationAssetData = {};
			data.m_bVegetationAssetLoaded = false;
			return false;
		}

		auto* registry = App::GetSubmodule<AssetRegistry>();
		auto* assetInfo =
			registry ? registry->GetAssetInfoPtr<LandscapeVegetationAssetInfoPtr>(data.m_vegetationAsset) : nullptr;
		if (!assetInfo)
		{
			SAILOR_LOG_ERROR(
				"LandscapeECS: vegetation asset %s is not registered.", data.m_vegetationAsset.ToString().c_str());
			return data.m_bVegetationAssetLoaded;
		}

		LandscapeVegetationAssetData loaded;
		std::string diagnostic;
		if (!loaded.Load(assetInfo->GetAssetFilepath(), diagnostic))
		{
			SAILOR_LOG_ERROR("LandscapeECS: cannot load vegetation asset %s: %s",
				data.m_vegetationAsset.ToString().c_str(),
				diagnostic.c_str());
			return data.m_bVegetationAssetLoaded;
		}
		if (loaded.m_chunksX != data.m_chunksX || loaded.m_chunksZ != data.m_chunksZ ||
			loaded.m_chunkSize != data.m_chunkSize)
		{
			SAILOR_LOG_ERROR("LandscapeECS: vegetation asset %s targets %ux%u chunks of %.3fm, but the component uses "
							 "%ux%u chunks of %.3fm.",
				data.m_vegetationAsset.ToString().c_str(),
				loaded.m_chunksX,
				loaded.m_chunksZ,
				loaded.m_chunkSize,
				data.m_chunksX,
				data.m_chunksZ,
				data.m_chunkSize);
			return data.m_bVegetationAssetLoaded;
		}

		const uint64_t instanceCount = loaded.GetInstanceCount();
		data.m_vegetationAssetData = std::move(loaded);
		data.m_bVegetationAssetLoaded = true;
		SAILOR_LOG("LandscapeECS: loaded %llu instances from vegetation asset %s.",
			static_cast<unsigned long long>(instanceCount),
			data.m_vegetationAsset.ToString().c_str());
		return true;
	}

	static bool BuildProceduralVegetationAssetData(const LandscapeData& data,
		LandscapeVegetationAssetData& result,
		std::string& outDiagnostic)
	{
		const size_t expectedChunkCount = static_cast<size_t>(data.m_chunksX) * data.m_chunksZ;
		if (data.m_chunks.Num() != expectedChunkCount)
		{
			outDiagnostic = "Landscape chunks are not ready for vegetation export.";
			return false;
		}

		result = {};
		result.m_chunksX = data.m_chunksX;
		result.m_chunksZ = data.m_chunksZ;
		result.m_profileCount = static_cast<uint32_t>(data.m_vegetationProfiles.Num());
		result.m_chunkSize = data.m_chunkSize;
		result.m_chunks.Resize(expectedChunkCount);
		const float landscapeWidth = data.m_chunksX * data.m_chunkSize;
		const float landscapeDepth = data.m_chunksZ * data.m_chunkSize;
		for (size_t chunkIndex = 0u; chunkIndex < expectedChunkCount; ++chunkIndex)
		{
			const auto& sourceChunk = data.m_chunks[chunkIndex];
			auto& chunk = result.m_chunks[chunkIndex];
			chunk.m_chunkX = static_cast<uint32_t>(chunkIndex % data.m_chunksX);
			chunk.m_chunkZ = static_cast<uint32_t>(chunkIndex / data.m_chunksX);
			const float originX = chunk.m_chunkX * data.m_chunkSize - landscapeWidth * 0.5f;
			const float originZ = chunk.m_chunkZ * data.m_chunkSize - landscapeDepth * 0.5f;
			for (size_t profileIndex = 0u; profileIndex < data.m_vegetationProfiles.Num(); ++profileIndex)
			{
				const auto& profile = data.m_vegetationProfiles[profileIndex];
				if (!profile.m_modelFileId)
				{
					continue;
				}
				chunk.m_instances.Reserve(chunk.m_instances.Num() + profile.m_instancesPerChunk);
				for (uint32_t instanceIndex = 0u; instanceIndex < profile.m_instancesPerChunk; ++instanceIndex)
				{
					const uint32_t randomSeed =
						data.m_seed ^ static_cast<uint32_t>(chunk.m_chunkX * 92821u + chunk.m_chunkZ * 68917u +
															profileIndex * 4099u + instanceIndex * 131u);
					const float positionX = originX + VegetationRandom01(randomSeed) * data.m_chunkSize;
					const float positionZ = originZ + VegetationRandom01(randomSeed + 1u) * data.m_chunkSize;
					chunk.m_instances.Add(BuildProceduralVegetationInstance(data,
						chunk.m_chunkX,
						chunk.m_chunkZ,
						profileIndex,
						instanceIndex,
						SampleLandscapeChunkHeight(data, sourceChunk, positionX, positionZ)));
				}
			}
		}
		if (!result.Validate(outDiagnostic))
		{
			return false;
		}
		result.RebuildRuntimeIndices();
		return true;
	}

	bool TrySaveVegetationAsset(LandscapeData& data)
	{
		if (!data.m_bSaveVegetationRequested)
		{
			return true;
		}
		data.m_bSaveVegetationRequested = false;
		if (!data.m_vegetationAsset)
		{
			SAILOR_LOG_ERROR("LandscapeECS: Save Vegetation requires a vegetation asset reference.");
			return false;
		}

		auto* registry = App::GetSubmodule<AssetRegistry>();
		auto* assetInfo =
			registry ? registry->GetAssetInfoPtr<LandscapeVegetationAssetInfoPtr>(data.m_vegetationAsset) : nullptr;
		if (!assetInfo || !assetInfo->IsWritable())
		{
			SAILOR_LOG_ERROR("LandscapeECS: vegetation asset %s is missing or read-only.",
				data.m_vegetationAsset.ToString().c_str());
			return false;
		}

		LandscapeVegetationAssetData generated;
		const LandscapeVegetationAssetData* source = nullptr;
		bool bGeneratedSource = false;
		std::string diagnostic;
		if (IsVegetationAssetCompatible(data))
		{
			source = &data.m_vegetationAssetData;
		}
		else if (BuildProceduralVegetationAssetData(data, generated, diagnostic))
		{
			source = &generated;
			bGeneratedSource = true;
		}
		if (!source)
		{
			SAILOR_LOG_ERROR("LandscapeECS: cannot prepare vegetation asset %s: %s",
				data.m_vegetationAsset.ToString().c_str(),
				diagnostic.c_str());
			return false;
		}
		if (!source->Save(assetInfo->GetAssetFilepath(), diagnostic))
		{
			SAILOR_LOG_ERROR("LandscapeECS: cannot save vegetation asset %s: %s",
				data.m_vegetationAsset.ToString().c_str(),
				diagnostic.c_str());
			return false;
		}
		const uint64_t savedInstanceCount = source->GetInstanceCount();
		if (bGeneratedSource)
		{
			data.m_vegetationAssetData = std::move(generated);
			data.m_bVegetationAssetLoaded = true;
		}
		data.m_bReloadVegetationAsset = false;
		SAILOR_LOG("LandscapeECS: saved %llu instances to vegetation asset %s.",
			static_cast<unsigned long long>(savedInstanceCount),
			data.m_vegetationAsset.ToString().c_str());
		return true;
	}

	uint64_t CalculateGrassViewRevision(const LandscapeData& data,
		uint32_t instanceCapacity,
		const glm::mat4& inverseOwnerMatrix,
		const TVector<glm::vec3>& cameraPositions)
	{
		const float meanInstanceSpacing =
			data.m_chunkSize / std::sqrt(static_cast<float>((std::max)(instanceCapacity, 1u)));
		const double cellSize = static_cast<double>((std::clamp)(meanInstanceSpacing, 0.5f, 4.0f));
		auto quantize = [cellSize](float value)
		{
			if (!std::isfinite(value))
			{
				return int64_t(0);
			}
			const double coordinate = std::floor(static_cast<double>(value) / cellSize);
			return static_cast<int64_t>((std::clamp)(coordinate,
				static_cast<double>((std::numeric_limits<int64_t>::lowest)()),
				static_cast<double>((std::numeric_limits<int64_t>::max)())));
		};

		size_t result = Fnv1aOffsetBasis;
		HashCombine(result, cameraPositions.Num());
		for (const glm::vec3& worldPosition : cameraPositions)
		{
			const glm::vec3 localPosition = glm::vec3(inverseOwnerMatrix * glm::vec4(worldPosition, 1.0f));
			HashCombine(result, quantize(localPosition.x), quantize(localPosition.y), quantize(localPosition.z));
		}
		return static_cast<uint64_t>(result) | 1ull;
	}

	LandscapeVegetationRenderInstances BuildGrassInstanceTransforms(const LandscapeData& data,
		const LandscapeChunk& chunk,
		size_t profileIndex,
		uint32_t instanceCount,
		const glm::mat4& ownerMatrix,
		const TVector<glm::vec3>& cameraPositions)
	{
		struct InstancePlacement final
		{
			LandscapeVegetationInstance m_instance{};
			float m_cameraDistanceSquared = 0.0f;
			uint32_t m_instanceIndex = 0u;
		};

		LandscapeVegetationRenderInstances result;
		if (profileIndex >= data.m_vegetationProfiles.Num())
		{
			return result;
		}
		const auto& profile = data.m_vegetationProfiles[profileIndex];
		const float landscapeWidth = data.m_chunksX * data.m_chunkSize;
		const float landscapeDepth = data.m_chunksZ * data.m_chunkSize;
		const float originX = chunk.m_chunkX * data.m_chunkSize - landscapeWidth * 0.5f;
		const float originZ = chunk.m_chunkZ * data.m_chunkSize - landscapeDepth * 0.5f;
		TVector<InstancePlacement> placements;
		if (const auto* authored = GetAuthoredVegetationChunk(data, chunk.m_chunkX, chunk.m_chunkZ))
		{
			placements.Reserve(authored->m_instances.Num());
			for (const auto& instance : authored->m_instances)
			{
				if (!instance.IsEnabled() || instance.m_profileIndex != profileIndex)
				{
					continue;
				}
				InstancePlacement placement;
				placement.m_instance = instance;
				placement.m_instanceIndex = static_cast<uint32_t>(placements.Num());
				placements.Add(std::move(placement));
			}
		}
		else
		{
			placements.Reserve(profile.m_instancesPerChunk);
			for (uint32_t instanceIndex = 0u; instanceIndex < profile.m_instancesPerChunk; ++instanceIndex)
			{
				const uint32_t randomSeed =
					data.m_seed ^ static_cast<uint32_t>(chunk.m_chunkX * 92821u + chunk.m_chunkZ * 68917u +
														profileIndex * 4099u + instanceIndex * 131u);
				const float positionX = originX + VegetationRandom01(randomSeed) * data.m_chunkSize;
				const float positionZ = originZ + VegetationRandom01(randomSeed + 1u) * data.m_chunkSize;
				InstancePlacement placement;
				placement.m_instance = BuildProceduralVegetationInstance(data,
					chunk.m_chunkX,
					chunk.m_chunkZ,
					profileIndex,
					instanceIndex,
					SampleLandscapeChunkHeight(data, chunk, positionX, positionZ));
				placement.m_instanceIndex = instanceIndex;
				placements.Add(std::move(placement));
			}
		}

		instanceCount = (std::min)(instanceCount, static_cast<uint32_t>(placements.Num()));
		result.m_transforms.Reserve(instanceCount);
		result.m_lodBiases.Reserve(instanceCount);
		result.m_cullDistanceScales.Reserve(instanceCount);
		result.m_shadowDistanceScales.Reserve(instanceCount);
		const bool bPartialSelection = instanceCount < placements.Num() && !cameraPositions.IsEmpty();
		if (bPartialSelection)
		{
			for (auto& placement : placements)
			{
				placement.m_cameraDistanceSquared = (std::numeric_limits<float>::infinity)();
				const glm::vec3 localPosition = glm::vec3(placement.m_instance.m_transform[3]);
				const glm::vec3 worldPosition = glm::vec3(ownerMatrix * glm::vec4(localPosition, 1.0f));
				for (const glm::vec3& cameraPosition : cameraPositions)
				{
					const glm::vec3 delta = worldPosition - cameraPosition;
					placement.m_cameraDistanceSquared =
						(std::min)(placement.m_cameraDistanceSquared, glm::dot(delta, delta));
				}
			}
			auto compareDistance = [](const InstancePlacement& lhs, const InstancePlacement& rhs)
			{
				if (lhs.m_cameraDistanceSquared != rhs.m_cameraDistanceSquared)
				{
					return lhs.m_cameraDistanceSquared < rhs.m_cameraDistanceSquared;
				}
				return lhs.m_instanceIndex < rhs.m_instanceIndex;
			};
			std::nth_element(placements.begin(), placements.begin() + instanceCount, placements.end(), compareDistance);
			placements.Resize(instanceCount);
			placements.Sort([](const InstancePlacement& lhs, const InstancePlacement& rhs)
				{ return lhs.m_instanceIndex < rhs.m_instanceIndex; });
		}
		else
		{
			placements.Resize(instanceCount);
		}
		for (const auto& placement : placements)
		{
			AppendRenderInstance(placement.m_instance, result);
		}
		return result;
	}
}
