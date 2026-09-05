#include "ECS/LandscapeECSInternal.h"

#include "AssetRegistry/Texture/TextureImporter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace Sailor::LandscapeECSInternal
{
	uint32_t HashVegetationSeed(uint32_t value)
	{
		value ^= value >> 16u;
		value *= 0x7feb352du;
		value ^= value >> 15u;
		value *= 0x846ca68bu;
		return value ^ (value >> 16u);
	}

	float VegetationRandom01(uint32_t value)
	{
		return static_cast<float>(HashVegetationSeed(value) & 0x00ffffffu) / 16777215.0f;
	}

	static uint64_t MakeVegetationStableId(uint32_t chunkX,
		uint32_t chunkZ,
		size_t profileIndex,
		uint32_t instanceIndex)
	{
		const uint64_t packed = (static_cast<uint64_t>(chunkX) & 0x3full) |
								((static_cast<uint64_t>(chunkZ) & 0x3full) << 6u) |
								((static_cast<uint64_t>(profileIndex) & 0xffffffffull) << 12u) |
								((static_cast<uint64_t>(instanceIndex) & 0x7ffull) << 44u);
		return packed + 1u;
	}

	LandscapeVegetationInstance BuildProceduralVegetationInstance(const LandscapeData& data,
		uint32_t chunkX,
		uint32_t chunkZ,
		size_t profileIndex,
		uint32_t instanceIndex,
		float height)
	{
		const auto& profile = data.m_vegetationProfiles[profileIndex];
		const float landscapeWidth = data.m_chunksX * data.m_chunkSize;
		const float landscapeDepth = data.m_chunksZ * data.m_chunkSize;
		const float originX = chunkX * data.m_chunkSize - landscapeWidth * 0.5f;
		const float originZ = chunkZ * data.m_chunkSize - landscapeDepth * 0.5f;
		const uint32_t randomSeed = data.m_seed ^ static_cast<uint32_t>(chunkX * 92821u + chunkZ * 68917u +
																		profileIndex * 4099u + instanceIndex * 131u);
		const glm::vec3 position(originX + VegetationRandom01(randomSeed) * data.m_chunkSize,
			height + profile.m_groundOffset,
			originZ + VegetationRandom01(randomSeed + 1u) * data.m_chunkSize);
		const float angle = VegetationRandom01(randomSeed + 2u) * glm::two_pi<float>();
		const float scale = glm::mix(profile.m_minScale, profile.m_maxScale, VegetationRandom01(randomSeed + 3u));

		LandscapeVegetationInstance result;
		result.m_stableId = MakeVegetationStableId(chunkX, chunkZ, profileIndex, instanceIndex);
		result.m_profileIndex = static_cast<uint32_t>(profileIndex);
		result.m_transform = glm::translate(glm::mat4(1.0f), position) *
							 glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0.0f, 1.0f, 0.0f)) *
							 glm::scale(glm::mat4(1.0f), glm::vec3(scale));
		return result;
	}

	bool IsVegetationAssetCompatible(const LandscapeData& data)
	{
		return data.m_bVegetationAssetLoaded && data.m_vegetationAssetData.m_chunksX == data.m_chunksX &&
			   data.m_vegetationAssetData.m_chunksZ == data.m_chunksZ &&
			   data.m_vegetationAssetData.m_chunkSize == data.m_chunkSize;
	}

	const LandscapeVegetationChunkData* GetAuthoredVegetationChunk(const LandscapeData& data,
		uint32_t chunkX,
		uint32_t chunkZ)
	{
		if (!IsVegetationAssetCompatible(data) || chunkX >= data.m_chunksX || chunkZ >= data.m_chunksZ)
		{
			return nullptr;
		}
		const size_t chunkIndex = static_cast<size_t>(chunkZ) * data.m_chunksX + chunkX;
		return chunkIndex < data.m_vegetationAssetData.m_chunks.Num() ? &data.m_vegetationAssetData.m_chunks[chunkIndex]
																	  : nullptr;
	}

	void AppendRenderInstance(const LandscapeVegetationInstance& instance, LandscapeVegetationRenderInstances& result)
	{
		if (!instance.IsEnabled())
		{
			return;
		}
		result.m_transforms.Add(instance.m_transform);
		result.m_lodBiases.Add(instance.m_lodBias);
		result.m_cullDistanceScales.Add(instance.m_cullDistanceScale);
		result.m_shadowDistanceScales.Add(instance.m_shadowDistanceScale);
	}

	uint32_t GetVegetationInstanceCapacity(const LandscapeData& data, const LandscapeChunk& chunk, size_t profileIndex)
	{
		if (const auto* authored = GetAuthoredVegetationChunk(data, chunk.m_chunkX, chunk.m_chunkZ))
		{
			if (profileIndex < authored->m_enabledInstancesPerProfile.Num())
			{
				return authored->m_enabledInstancesPerProfile[profileIndex];
			}
			uint32_t result = 0u;
			for (const auto& instance : authored->m_instances)
			{
				result += instance.IsEnabled() && instance.m_profileIndex == profileIndex ? 1u : 0u;
			}
			return result;
		}
		return profileIndex < data.m_vegetationProfiles.Num()
				   ? data.m_vegetationProfiles[profileIndex].m_instancesPerChunk
				   : 0u;
	}

	static float SmoothNoise(int32_t x, int32_t z, uint32_t seed)
	{
		const uint32_t key = static_cast<uint32_t>(x) * 0x9e3779b9u ^ static_cast<uint32_t>(z) * 0x85ebca6bu ^ seed;
		return VegetationRandom01(key) * 2.0f - 1.0f;
	}

	static float Fade(float value)
	{
		return value * value * (3.0f - 2.0f * value);
	}

	static float ValueNoise(float x, float z, uint32_t seed)
	{
		const int32_t x0 = static_cast<int32_t>(std::floor(x));
		const int32_t z0 = static_cast<int32_t>(std::floor(z));
		const float tx = Fade(x - static_cast<float>(x0));
		const float tz = Fade(z - static_cast<float>(z0));
		const float a = glm::mix(SmoothNoise(x0, z0, seed), SmoothNoise(x0 + 1, z0, seed), tx);
		const float b = glm::mix(SmoothNoise(x0, z0 + 1, seed), SmoothNoise(x0 + 1, z0 + 1, seed), tx);
		return glm::mix(a, b, tz);
	}

	bool DecodeCpuTexture(const FileId& fileId, LandscapeCpuTexture& result)
	{
		if (!fileId)
		{
			return false;
		}
		uint32_t mipLevels = 1u;
		if (!TextureImporter::DecodeTextureCpu(fileId, result.m_pixels, result.m_width, result.m_height, mipLevels))
		{
			return false;
		}
		const size_t pixelCount = static_cast<size_t>(result.m_width) * result.m_height;
		result.m_bFloat = pixelCount > 0u && result.m_pixels.Num() == pixelCount * sizeof(float) * 4u;
		return result.IsValid();
	}

	static float ReadTextureChannel(const LandscapeCpuTexture& texture, int32_t x, int32_t y, uint32_t channel)
	{
		x = (std::clamp)(x, 0, texture.m_width - 1);
		y = (std::clamp)(y, 0, texture.m_height - 1);
		channel = (std::min)(channel, 3u);
		const size_t pixel = static_cast<size_t>(y) * texture.m_width + x;
		if (!texture.m_bFloat)
		{
			return static_cast<float>(texture.m_pixels[pixel * 4u + channel]) / 255.0f;
		}

		float value = 0.0f;
		memcpy(&value, texture.m_pixels.GetData() + (pixel * 4u + channel) * sizeof(float), sizeof(float));
		return (std::clamp)(value, 0.0f, 1.0f);
	}

	static float SampleTextureChannel(const LandscapeCpuTexture& texture, float u, float v, uint32_t channel)
	{
		if (!texture.IsValid())
		{
			return 0.0f;
		}
		const float x = (std::clamp)(u, 0.0f, 1.0f) * static_cast<float>(texture.m_width - 1);
		const float y = (std::clamp)(v, 0.0f, 1.0f) * static_cast<float>(texture.m_height - 1);
		const int32_t x0 = static_cast<int32_t>(std::floor(x));
		const int32_t y0 = static_cast<int32_t>(std::floor(y));
		const float tx = x - x0;
		const float ty = y - y0;
		const float a = glm::mix(
			ReadTextureChannel(texture, x0, y0, channel), ReadTextureChannel(texture, x0 + 1, y0, channel), tx);
		const float b = glm::mix(
			ReadTextureChannel(texture, x0, y0 + 1, channel), ReadTextureChannel(texture, x0 + 1, y0 + 1, channel), tx);
		return glm::mix(a, b, ty);
	}

	static float BrushFalloff(float x, float z, const TVector<float>& stamps, size_t offset)
	{
		const float radius = (std::max)(stamps[offset + 2u], 0.001f);
		const float distance = glm::distance(glm::vec2(x, z), glm::vec2(stamps[offset], stamps[offset + 1u]));
		const float linear = (std::clamp)(1.0f - distance / radius, 0.0f, 1.0f);
		return linear * linear * (3.0f - 2.0f * linear);
	}

	static float SampleHeight(float x,
		float z,
		float landscapeWidth,
		float landscapeDepth,
		float noiseScale,
		float heightScale,
		uint32_t seed,
		const TVector<float>& sculptStamps,
		const LandscapeCpuTexture& heightmap)
	{
		float height = 0.0f;
		if (heightmap.IsValid())
		{
			const float u = x / landscapeWidth + 0.5f;
			const float v = z / landscapeDepth + 0.5f;
			height = (SampleTextureChannel(heightmap, u, v, 0u) * 2.0f - 1.0f) * heightScale;
		}
		else
		{
			float amplitude = 1.0f;
			float frequency = noiseScale;
			float sum = 0.0f;
			float normalization = 0.0f;
			for (uint32_t octave = 0; octave < 4u; ++octave)
			{
				sum += ValueNoise(x * frequency, z * frequency, seed + octave * 1013u) * amplitude;
				normalization += amplitude;
				amplitude *= 0.5f;
				frequency *= 2.0f;
			}
			height = heightScale > 0.0f ? sum / normalization * heightScale : 0.0f;
		}
		for (size_t stamp = 0u; stamp + 4u < sculptStamps.Num(); stamp += 5u)
		{
			const float falloff = BrushFalloff(x, z, sculptStamps, stamp);
			const float strength = sculptStamps[stamp + 3u] * falloff;
			const uint32_t operation = static_cast<uint32_t>(sculptStamps[stamp + 4u]);
			if (operation == 0u)
				height += strength;
			else if (operation == 1u)
				height -= strength;
			else
				height = glm::mix(height, 0.0f, (std::clamp)(strength, 0.0f, 1.0f));
		}
		return height;
	}

	static void BuildLandscapeLodGeometry(const LandscapeData& data, LandscapeChunkCpuData& result)
	{
		const uint32_t resolution = data.m_chunkResolution;
		const uint32_t row = resolution + 1u;
		const uint32_t baseVertexCount = row * row;
		if (data.m_lodSkirtDepth > 0.0f)
		{
			result.m_vertices.Reserve(result.m_vertices.Num() + static_cast<size_t>(row) * 4u);
			auto appendSkirtVertex = [&result, &data](uint32_t sourceIndex)
			{
				auto vertex = result.m_vertices[sourceIndex];
				vertex.m_position.y -= data.m_lodSkirtDepth;
				result.m_localBounds.Extend(Math::AABB(vertex.m_position, glm::vec3(0.01f)));
				result.m_vertices.Add(std::move(vertex));
			};
			for (uint32_t x = 0u; x <= resolution; ++x)
			{
				appendSkirtVertex(x);
			}
			for (uint32_t x = 0u; x <= resolution; ++x)
			{
				appendSkirtVertex(resolution * row + x);
			}
			for (uint32_t z = 0u; z <= resolution; ++z)
			{
				appendSkirtVertex(z * row);
			}
			for (uint32_t z = 0u; z <= resolution; ++z)
			{
				appendSkirtVertex(z * row + resolution);
			}
		}

		const auto collisionCoordinates = BuildLandscapeLodCoordinates(resolution, 1u);
		AppendLandscapeLodIndices(resolution, collisionCoordinates, false, result.m_collisionIndices);
		uint32_t previousCoordinateCount = 0u;
		uint32_t stride = 1u;
		const size_t maxLods = data.m_lodDistances.Num() + 1u;
		for (size_t lod = 0u; lod < maxLods; ++lod)
		{
			const auto coordinates = BuildLandscapeLodCoordinates(resolution, stride);
			if (coordinates.Num() < 2u || (lod > 0u && coordinates.Num() == previousCoordinateCount))
			{
				break;
			}
			previousCoordinateCount = static_cast<uint32_t>(coordinates.Num());
			result.m_lodFirstIndices.Add(static_cast<uint32_t>(result.m_indices.Num()));
			AppendLandscapeLodIndices(resolution,
				coordinates,
				data.m_lodSkirtDepth > 0.0f &&
					result.m_vertices.Num() >= static_cast<size_t>(baseVertexCount) + row * 4u,
				result.m_indices);
			result.m_lodIndexCounts.Add(
				static_cast<uint32_t>(result.m_indices.Num()) - *result.m_lodFirstIndices.Last());
			stride *= 2u;
		}
	}

	LandscapeChunkCpuData BuildChunk(const LandscapeData& data,
		const LandscapeCpuTexture& heightmap,
		const TVector<LandscapeCpuTexture>& materialMasks,
		uint32_t chunkX,
		uint32_t chunkZ)
	{
		LandscapeChunkCpuData result;
		result.m_chunkX = chunkX;
		result.m_chunkZ = chunkZ;
		const uint32_t resolution = data.m_chunkResolution;
		const uint32_t row = resolution + 1u;
		const float step = data.m_chunkSize / static_cast<float>(resolution);
		const float landscapeWidth = data.m_chunksX * data.m_chunkSize;
		const float landscapeDepth = data.m_chunksZ * data.m_chunkSize;
		const float originX = chunkX * data.m_chunkSize - landscapeWidth * 0.5f;
		const float originZ = chunkZ * data.m_chunkSize - landscapeDepth * 0.5f;

		result.m_vertices.Resize(static_cast<size_t>(row) * row);
		for (uint32_t z = 0u; z <= resolution; ++z)
		{
			for (uint32_t x = 0u; x <= resolution; ++x)
			{
				const float localX = originX + x * step;
				const float localZ = originZ + z * step;
				const auto sampleHeight = [&](float sampleX, float sampleZ)
				{
					return SampleHeight(sampleX,
						sampleZ,
						landscapeWidth,
						landscapeDepth,
						data.m_noiseScale,
						data.m_heightScale,
						data.m_seed,
						data.m_sculptStamps,
						heightmap);
				};
				const float height = sampleHeight(localX, localZ);
				const float left = sampleHeight(localX - step, localZ);
				const float right = sampleHeight(localX + step, localZ);
				const float down = sampleHeight(localX, localZ - step);
				const float up = sampleHeight(localX, localZ + step);
				const glm::vec3 normal = glm::normalize(glm::vec3(left - right, 2.0f * step, down - up));
				const glm::vec3 tangent = glm::normalize(glm::vec3(2.0f * step, right - left, 0.0f));
				const glm::vec3 bitangent = glm::normalize(glm::cross(normal, tangent));
				const float slope = 1.0f - (std::clamp)(normal.y, 0.0f, 1.0f);
				const float normalizedHeight =
					data.m_heightScale > 0.0f ? (std::clamp)(height / (data.m_heightScale * 2.0f) + 0.5f, 0.0f, 1.0f)
											  : 0.5f;
				glm::vec4 weights;
				weights.x = (1.0f - slope) * (1.0f - normalizedHeight);
				weights.y = slope;
				weights.z = (1.0f - slope) * normalizedHeight;
				weights.w = (std::max)(0.0f, normalizedHeight - 0.72f) * 3.57f;
				weights /= (std::max)(glm::dot(weights, glm::vec4(1.0f)), 0.0001f);
				if (!materialMasks.IsEmpty())
				{
					const float u = localX / landscapeWidth + 0.5f;
					const float v = localZ / landscapeDepth + 0.5f;
					glm::vec4 importedWeights(0.0f);
					if (materialMasks.Num() == 1u)
					{
						for (uint32_t channel = 0u; channel < 4u; ++channel)
						{
							importedWeights[channel] = SampleTextureChannel(materialMasks[0], u, v, channel);
						}
					}
					else
					{
						for (size_t mask = 0u; mask < materialMasks.Num() && mask < 4u; ++mask)
						{
							importedWeights[static_cast<glm::length_t>(mask)] =
								SampleTextureChannel(materialMasks[mask], u, v, 0u);
						}
					}
					const float importedWeightSum = glm::dot(importedWeights, glm::vec4(1.0f));
					if (importedWeightSum > 0.0001f)
					{
						weights = importedWeights / importedWeightSum;
					}
				}
				for (size_t stamp = 0u; stamp + 4u < data.m_paintStamps.Num(); stamp += 5u)
				{
					const float alpha = (std::clamp)(data.m_paintStamps[stamp + 3u] *
														 BrushFalloff(localX, localZ, data.m_paintStamps, stamp),
						0.0f,
						1.0f);
					glm::vec4 target(0.0f);
					target[(std::min)(static_cast<uint32_t>(data.m_paintStamps[stamp + 4u]), 3u)] = 1.0f;
					weights = glm::mix(weights, target, alpha);
				}

				auto& vertex = result.m_vertices[static_cast<size_t>(z) * row + x];
				vertex.m_position = glm::vec3(localX, height, localZ);
				vertex.m_normal = normal;
				vertex.m_tangent = tangent;
				vertex.m_bitangent = bitangent;
				vertex.m_texcoord = glm::vec2(localX, localZ) * data.m_textureTiling;
				vertex.m_color = weights;
				result.m_localBounds.Extend(Math::AABB(vertex.m_position, glm::vec3(0.01f)));
			}
		}

		BuildLandscapeLodGeometry(data, result);

		result.m_bakeTriangles.Reserve(result.m_collisionIndices.Num() / 3u);
		for (size_t index = 0u; index + 2u < result.m_collisionIndices.Num(); index += 3u)
		{
			Math::Triangle triangle{};
			for (size_t vertex = 0u; vertex < 3u; ++vertex)
			{
				const auto& source = result.m_vertices[result.m_collisionIndices[index + vertex]];
				triangle.m_vertices[vertex] = source.m_position;
				triangle.m_normals[vertex] = source.m_normal;
				triangle.m_tangent[vertex] = source.m_tangent;
				triangle.m_bitangent[vertex] = source.m_bitangent;
				triangle.m_uvs[vertex] = source.m_texcoord;
				triangle.m_uvs2[vertex] = source.m_texcoord;
				triangle.m_colors[vertex] = source.m_color;
			}
			triangle.m_materialIndex = 0u;
			triangle.m_centroid = (triangle.m_vertices[0] + triangle.m_vertices[1] + triangle.m_vertices[2]) / 3.0f;
			result.m_bakeTriangles.Add(std::move(triangle));
		}

		for (size_t profileIndex = 0u; profileIndex < data.m_vegetationProfiles.Num(); ++profileIndex)
		{
			const auto& profile = data.m_vegetationProfiles[profileIndex];
			if (!profile.m_modelFileId || profile.m_residency == ELandscapeVegetationResidency::Grass)
			{
				continue;
			}
			if (const auto* authored = GetAuthoredVegetationChunk(data, chunkX, chunkZ))
			{
				for (const auto& instance : authored->m_instances)
				{
					if (instance.IsEnabled() && instance.m_profileIndex == profileIndex)
					{
						result.m_vegetation.Add(instance);
					}
				}
				continue;
			}
			result.m_vegetation.Reserve(result.m_vegetation.Num() + profile.m_instancesPerChunk);
			for (uint32_t instance = 0u; instance < profile.m_instancesPerChunk; ++instance)
			{
				const uint32_t randomSeed = data.m_seed ^ static_cast<uint32_t>(chunkX * 92821u + chunkZ * 68917u +
																				profileIndex * 4099u + instance * 131u);
				const float positionX = originX + VegetationRandom01(randomSeed) * data.m_chunkSize;
				const float positionZ = originZ + VegetationRandom01(randomSeed + 1u) * data.m_chunkSize;
				const float height = SampleHeight(positionX,
					positionZ,
					landscapeWidth,
					landscapeDepth,
					data.m_noiseScale,
					data.m_heightScale,
					data.m_seed,
					data.m_sculptStamps,
					heightmap);
				result.m_vegetation.Add(
					BuildProceduralVegetationInstance(data, chunkX, chunkZ, profileIndex, instance, height));
			}
		}
		return result;
	}

}
