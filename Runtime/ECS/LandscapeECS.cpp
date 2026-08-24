#include "ECS/LandscapeECS.h"
#include "ECS/LandscapeStreaming.h"

#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/AssetRegistry.h"
#include "Components/LandscapeComponent.h"
#include "ECS/CameraECS.h"
#include "ECS/TransformECS.h"
#include "ECS/PhysicsECS.h"
#include "Engine/GameObject.h"
#include "Math/Transform.h"
#include "RHI/GraphicsDriver.h"
#include "RHI/Renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

using namespace Sailor;
using namespace Sailor::Tasks;

namespace
{
	struct LandscapeCpuTexture
	{
		TVector<uint8_t> m_pixels{};
		int32_t m_width = 0;
		int32_t m_height = 0;
		bool m_bFloat = false;

		bool IsValid() const
		{
			return m_width > 0 && m_height > 0 &&
				m_pixels.Num() >= static_cast<size_t>(m_width) * m_height * (m_bFloat ? 16u : 4u);
		}
	};

	struct LandscapeChunkCpuData
	{
		uint32_t m_chunkX = 0u;
		uint32_t m_chunkZ = 0u;
		TVector<RHI::VertexP3N3T3B3UV2C4> m_vertices{};
		TVector<uint32_t> m_indices{};
		TVector<uint32_t> m_collisionIndices{};
		TVector<uint32_t> m_lodFirstIndices{};
		TVector<uint32_t> m_lodIndexCounts{};
		TVector<LandscapeVegetationInstance> m_vegetation{};
		Math::AABB m_localBounds{};
	};

	uint32_t Hash(uint32_t value)
	{
		value ^= value >> 16u;
		value *= 0x7feb352du;
		value ^= value >> 15u;
		value *= 0x846ca68bu;
		return value ^ (value >> 16u);
	}

	float Random01(uint32_t value)
	{
		return static_cast<float>(Hash(value) & 0x00ffffffu) / 16777215.0f;
	}

	uint64_t MakeVegetationStableId(
		uint32_t chunkX,
		uint32_t chunkZ,
		size_t profileIndex,
		uint32_t instanceIndex)
	{
		const uint64_t packed =
			(static_cast<uint64_t>(chunkX) & 0x3full) |
			((static_cast<uint64_t>(chunkZ) & 0x3full) << 6u) |
			((static_cast<uint64_t>(profileIndex) & 0xffffffffull) << 12u) |
			((static_cast<uint64_t>(instanceIndex) & 0x7ffull) << 44u);
		return packed + 1u;
	}

	LandscapeVegetationInstance BuildProceduralVegetationInstance(
		const LandscapeData& data,
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
		const uint32_t randomSeed = data.m_seed ^
			static_cast<uint32_t>(chunkX * 92821u + chunkZ * 68917u +
				profileIndex * 4099u + instanceIndex * 131u);
		const glm::vec3 position(
			originX + Random01(randomSeed) * data.m_chunkSize,
			height + profile.m_groundOffset,
			originZ + Random01(randomSeed + 1u) * data.m_chunkSize);
		const float angle = Random01(randomSeed + 2u) * glm::two_pi<float>();
		const float scale = glm::mix(
			profile.m_minScale,
			profile.m_maxScale,
			Random01(randomSeed + 3u));

		LandscapeVegetationInstance result;
		result.m_stableId = MakeVegetationStableId(
			chunkX,
			chunkZ,
			profileIndex,
			instanceIndex);
		result.m_profileIndex = static_cast<uint32_t>(profileIndex);
		result.m_transform =
			glm::translate(glm::mat4(1.0f), position) *
			glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0.0f, 1.0f, 0.0f)) *
			glm::scale(glm::mat4(1.0f), glm::vec3(scale));
		return result;
	}

	bool IsVegetationAssetCompatible(const LandscapeData& data)
	{
		return data.m_bVegetationAssetLoaded &&
			data.m_vegetationAssetData.m_chunksX == data.m_chunksX &&
			data.m_vegetationAssetData.m_chunksZ == data.m_chunksZ &&
			data.m_vegetationAssetData.m_chunkSize == data.m_chunkSize;
	}

	const LandscapeVegetationChunkData* GetAuthoredVegetationChunk(
		const LandscapeData& data,
		uint32_t chunkX,
		uint32_t chunkZ)
	{
		if (!IsVegetationAssetCompatible(data) ||
			chunkX >= data.m_chunksX || chunkZ >= data.m_chunksZ)
		{
			return nullptr;
		}
		const size_t chunkIndex = static_cast<size_t>(chunkZ) * data.m_chunksX + chunkX;
		return chunkIndex < data.m_vegetationAssetData.m_chunks.Num() ?
			&data.m_vegetationAssetData.m_chunks[chunkIndex] : nullptr;
	}

	void AppendRenderInstance(
		const LandscapeVegetationInstance& instance,
		LandscapeVegetationRenderInstances& result)
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

	uint32_t GetVegetationInstanceCapacity(
		const LandscapeData& data,
		const LandscapeChunk& chunk,
		size_t profileIndex)
	{
		if (const auto* authored = GetAuthoredVegetationChunk(
				data,
				chunk.m_chunkX,
				chunk.m_chunkZ))
		{
			if (profileIndex < authored->m_enabledInstancesPerProfile.Num())
			{
				return authored->m_enabledInstancesPerProfile[profileIndex];
			}
			uint32_t result = 0u;
			for (const auto& instance : authored->m_instances)
			{
				result += instance.IsEnabled() &&
					instance.m_profileIndex == profileIndex ? 1u : 0u;
			}
			return result;
		}
		return profileIndex < data.m_vegetationProfiles.Num() ?
			data.m_vegetationProfiles[profileIndex].m_instancesPerChunk : 0u;
	}

	float SmoothNoise(int32_t x, int32_t z, uint32_t seed)
	{
		const uint32_t key = static_cast<uint32_t>(x) * 0x9e3779b9u ^
			static_cast<uint32_t>(z) * 0x85ebca6bu ^ seed;
		return Random01(key) * 2.0f - 1.0f;
	}

	float Fade(float value)
	{
		return value * value * (3.0f - 2.0f * value);
	}

	float ValueNoise(float x, float z, uint32_t seed)
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
		if (!TextureImporter::DecodeTextureCpu(fileId, result.m_pixels,
			result.m_width, result.m_height, mipLevels))
		{
			return false;
		}
		const size_t pixelCount = static_cast<size_t>(result.m_width) * result.m_height;
		result.m_bFloat = pixelCount > 0u && result.m_pixels.Num() == pixelCount * sizeof(float) * 4u;
		return result.IsValid();
	}

	float ReadTextureChannel(const LandscapeCpuTexture& texture, int32_t x, int32_t y, uint32_t channel)
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
		memcpy(&value, texture.m_pixels.GetData() +
			(pixel * 4u + channel) * sizeof(float), sizeof(float));
		return (std::clamp)(value, 0.0f, 1.0f);
	}

	float SampleTextureChannel(const LandscapeCpuTexture& texture, float u, float v, uint32_t channel)
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
		const float a = glm::mix(ReadTextureChannel(texture, x0, y0, channel),
			ReadTextureChannel(texture, x0 + 1, y0, channel), tx);
		const float b = glm::mix(ReadTextureChannel(texture, x0, y0 + 1, channel),
			ReadTextureChannel(texture, x0 + 1, y0 + 1, channel), tx);
		return glm::mix(a, b, ty);
	}

	float BrushFalloff(float x, float z, const TVector<float>& stamps, size_t offset)
	{
		const float radius = (std::max)(stamps[offset + 2u], 0.001f);
		const float distance = glm::distance(glm::vec2(x, z), glm::vec2(stamps[offset], stamps[offset + 1u]));
		const float linear = (std::clamp)(1.0f - distance / radius, 0.0f, 1.0f);
		return linear * linear * (3.0f - 2.0f * linear);
	}

	float SampleHeight(float x, float z, float landscapeWidth, float landscapeDepth,
		float noiseScale, float heightScale, uint32_t seed,
		const TVector<float>& sculptStamps, const LandscapeCpuTexture& heightmap)
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
			if (operation == 0u) height += strength;
			else if (operation == 1u) height -= strength;
			else height = glm::mix(height, 0.0f, (std::clamp)(strength, 0.0f, 1.0f));
		}
		return height;
	}

	void BuildLandscapeLodGeometry(
		const LandscapeData& data,
		LandscapeChunkCpuData& result)
	{
		const uint32_t resolution = data.m_chunkResolution;
		const uint32_t row = resolution + 1u;
		const uint32_t baseVertexCount = row * row;
		if (data.m_lodSkirtDepth > 0.0f)
		{
			result.m_vertices.Reserve(
				result.m_vertices.Num() + static_cast<size_t>(row) * 4u);
			auto appendSkirtVertex = [&result, &data](uint32_t sourceIndex)
				{
					auto vertex = result.m_vertices[sourceIndex];
					vertex.m_position.y -= data.m_lodSkirtDepth;
					result.m_localBounds.Extend(
						Math::AABB(vertex.m_position, glm::vec3(0.01f)));
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
		AppendLandscapeLodIndices(
			resolution,
			collisionCoordinates,
			false,
			result.m_collisionIndices);
		uint32_t previousCoordinateCount = 0u;
		uint32_t stride = 1u;
		const size_t maxLods = data.m_lodDistances.Num() + 1u;
		for (size_t lod = 0u; lod < maxLods; ++lod)
		{
			const auto coordinates = BuildLandscapeLodCoordinates(resolution, stride);
			if (coordinates.Num() < 2u ||
				(lod > 0u && coordinates.Num() == previousCoordinateCount))
			{
				break;
			}
			previousCoordinateCount = static_cast<uint32_t>(coordinates.Num());
			result.m_lodFirstIndices.Add(static_cast<uint32_t>(result.m_indices.Num()));
			AppendLandscapeLodIndices(
				resolution,
				coordinates,
				data.m_lodSkirtDepth > 0.0f &&
					result.m_vertices.Num() >= static_cast<size_t>(baseVertexCount) + row * 4u,
				result.m_indices);
			result.m_lodIndexCounts.Add(
				static_cast<uint32_t>(result.m_indices.Num()) -
				*result.m_lodFirstIndices.Last());
			stride *= 2u;
		}
	}

	LandscapeChunkCpuData BuildChunk(const LandscapeData& data,
		const LandscapeCpuTexture& heightmap,
		const TVector<LandscapeCpuTexture>& materialMasks,
		uint32_t chunkX, uint32_t chunkZ)
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
					return SampleHeight(sampleX, sampleZ, landscapeWidth, landscapeDepth,
						data.m_noiseScale, data.m_heightScale, data.m_seed,
						data.m_sculptStamps, heightmap);
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
				const float normalizedHeight = data.m_heightScale > 0.0f ?
					(std::clamp)(height / (data.m_heightScale * 2.0f) + 0.5f, 0.0f, 1.0f) : 0.5f;
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
						BrushFalloff(localX, localZ, data.m_paintStamps, stamp), 0.0f, 1.0f);
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

		for (size_t profileIndex = 0u; profileIndex < data.m_vegetationProfiles.Num(); ++profileIndex)
		{
			const auto& profile = data.m_vegetationProfiles[profileIndex];
			if (!profile.m_modelFileId ||
				profile.m_residency == ELandscapeVegetationResidency::Grass)
			{
				continue;
			}
			if (const auto* authored = GetAuthoredVegetationChunk(
					data,
					chunkX,
					chunkZ))
			{
				for (const auto& instance : authored->m_instances)
				{
					if (instance.IsEnabled() &&
						instance.m_profileIndex == profileIndex)
					{
						result.m_vegetation.Add(instance);
					}
				}
				continue;
			}
			result.m_vegetation.Reserve(result.m_vegetation.Num() + profile.m_instancesPerChunk);
			for (uint32_t instance = 0u; instance < profile.m_instancesPerChunk; ++instance)
			{
				const uint32_t randomSeed = data.m_seed ^
					static_cast<uint32_t>(chunkX * 92821u + chunkZ * 68917u +
						profileIndex * 4099u + instance * 131u);
				const float positionX = originX + Random01(randomSeed) * data.m_chunkSize;
				const float positionZ = originZ + Random01(randomSeed + 1u) * data.m_chunkSize;
				const float height = SampleHeight(positionX, positionZ,
					landscapeWidth, landscapeDepth, data.m_noiseScale, data.m_heightScale,
					data.m_seed, data.m_sculptStamps, heightmap);
				result.m_vegetation.Add(BuildProceduralVegetationInstance(
					data,
					chunkX,
					chunkZ,
					profileIndex,
					instance,
					height));
			}
		}
		return result;
	}

	float GetProfileValue(const TVector<float>& values, size_t index, float fallback)
	{
		return index < values.Num() && std::isfinite(values[index]) ? values[index] : fallback;
	}

	void AppendShadowMesh(RHI::RHIShadowCasterProxy& shadowCaster,
		const RHI::RHIMeshPtr& mesh, const glm::mat4& worldMatrix,
		const MaterialPtr& material, float maxCameraDistance)
	{
		if (!mesh || !material)
		{
			return;
		}

		const size_t opaqueQueueTag = GetHash(std::string("Opaque"));
		const size_t maskedQueueTag = GetHash(std::string("Masked"));
		const size_t renderQueueTag = material->GetRenderState().GetTag();
		if (renderQueueTag != opaqueQueueTag && renderQueueTag != maskedQueueTag)
		{
			return;
		}

		RHI::RHIShadowMeshProxy shadowMesh;
		shadowMesh.m_mesh = mesh;
		shadowMesh.m_worldMatrix = worldMatrix;
		shadowMesh.m_renderQueueTag = renderQueueTag;
		shadowMesh.m_maxCameraDistance = maxCameraDistance;
		if (material->GetRenderState().IsRequiredCustomDepthShader())
		{
			auto rhiMaterialSource = material;
			shadowMesh.m_customDepthMaterial = rhiMaterialSource->GetOrAddRHI(
				mesh->m_vertexDescription);
			shadowMesh.m_customDepthShader = material->GetShader();
		}

		auto* textureImporter = App::GetSubmodule<TextureImporter>();
		if (renderQueueTag == maskedQueueTag)
		{
			const glm::vec4* baseColorFactor = nullptr;
			if (!material->GetUniformsVec4().Find("material.baseColorFactor", baseColorFactor))
			{
				material->GetUniformsVec4().Find("material.albedo", baseColorFactor);
			}
			if (baseColorFactor)
			{
				shadowMesh.m_baseColorFactor = *baseColorFactor;
			}

			const float* alphaCutoff = nullptr;
			if (material->GetUniformsFloat().Find("material.alphaCutoff", alphaCutoff) && alphaCutoff)
			{
				shadowMesh.m_alphaCutoff = *alphaCutoff;
			}

			const TexturePtr* baseColorTexture = nullptr;
			if (!material->GetSamplers().Find("baseColorSampler", baseColorTexture))
			{
				material->GetSamplers().Find("albedoSampler", baseColorTexture);
			}
			if (textureImporter && baseColorTexture && *baseColorTexture)
			{
				shadowMesh.m_baseColorSampler = static_cast<uint32_t>(
					textureImporter->GetTextureIndex((*baseColorTexture)->GetFileId()));
			}
		}
#if defined(__APPLE__)
		shadowMesh.m_materialTextureSamplers.Insert(0u);
		if (textureImporter)
		{
			for (const auto& sampler : material->GetSamplers())
			{
				shadowMesh.m_materialTextureSamplers.Insert(sampler.m_second ?
					static_cast<uint32_t>(textureImporter->GetTextureIndex(sampler.m_second->GetFileId())) : 0u);
			}
		}
#endif
		shadowCaster.m_meshes.Add(std::move(shadowMesh));
	}

	void AppendDepthMaterialMetadata(RHI::RHISceneViewProxy& proxy, const MaterialPtr& material)
	{
		glm::vec4 baseColorFactor{ 1.0f };
		float alphaCutoff = 0.5f;
		uint32_t baseColorSampler = 0u;
		if (material->GetRenderState().GetTag() != GetHash(std::string("Masked")))
		{
			proxy.m_baseColorFactors.Add(baseColorFactor);
			proxy.m_alphaCutoffs.Add(alphaCutoff);
			proxy.m_baseColorSamplers.Add(baseColorSampler);
			return;
		}

		const glm::vec4* materialBaseColorFactor = nullptr;
		if (!material->GetUniformsVec4().Find("material.baseColorFactor", materialBaseColorFactor))
		{
			material->GetUniformsVec4().Find("material.albedo", materialBaseColorFactor);
		}
		if (materialBaseColorFactor)
		{
			baseColorFactor = *materialBaseColorFactor;
		}
		proxy.m_baseColorFactors.Add(baseColorFactor);

		const float* materialAlphaCutoff = nullptr;
		if (material->GetUniformsFloat().Find("material.alphaCutoff", materialAlphaCutoff) && materialAlphaCutoff)
		{
			alphaCutoff = *materialAlphaCutoff;
		}
		proxy.m_alphaCutoffs.Add(alphaCutoff);

		const TexturePtr* baseColorTexture = nullptr;
		if (!material->GetSamplers().Find("baseColorSampler", baseColorTexture))
		{
			material->GetSamplers().Find("albedoSampler", baseColorTexture);
		}
		auto* textureImporter = App::GetSubmodule<TextureImporter>();
		if (textureImporter && baseColorTexture && *baseColorTexture)
		{
			baseColorSampler = static_cast<uint32_t>(
				textureImporter->GetTextureIndex((*baseColorTexture)->GetFileId()));
		}
		proxy.m_baseColorSamplers.Add(baseColorSampler);
	}

	void GetOctreeBounds(const Math::AABB& bounds, glm::ivec3& center, glm::ivec3& extents)
	{
		const glm::ivec3 minimum = glm::ivec3(glm::floor(bounds.m_min));
		const glm::ivec3 maximum = glm::ivec3(glm::ceil(bounds.m_max));
		center = minimum + (maximum - minimum) / 2;
		extents = glm::max(glm::max(maximum - center, center - minimum), glm::ivec3(1));
	}

	size_t LandscapeProxyId(size_t componentIndex, size_t chunkIndex)
	{
		return (size_t(1) << (sizeof(size_t) * 8u - 1u)) |
			((componentIndex & 0x7fffffffu) << 24u) | (chunkIndex & 0xffffffu);
	}

	size_t LandscapeVegetationProxyId(
		size_t componentIndex,
		size_t chunkIndex,
		size_t instanceIndex)
	{
		return (size_t(3) << (sizeof(size_t) * 8u - 2u)) |
			((componentIndex & 0xfffffu) << 36u) |
			((chunkIndex & 0xfffffu) << 16u) | (instanceIndex & 0xffffu);
	}

	enum class EVegetationProxyBuildResult : uint8_t
	{
		Success,
		Pending,
		NoRenderData
	};

	EVegetationProxyBuildResult BuildLandscapeVegetationProxy(
		size_t componentIndex,
		size_t chunkIndex,
		size_t profileIndex,
		const LandscapeVegetationProfile& profile,
		const glm::mat4& ownerMatrix,
		uint64_t frame,
		LandscapeVegetationRenderInstances instances,
		EMobilityType mobility,
		uint64_t revision,
		LandscapeVegetationRenderProxy& result)
	{
		const bool bUseMaterialOverride = static_cast<bool>(profile.m_materialFileId);
		if (!profile.m_model || !profile.m_model->IsReady() ||
			(bUseMaterialOverride && (!profile.m_material || !profile.m_material->IsReady())))
		{
			return EVegetationProxyBuildResult::Pending;
		}

		TVector<RHI::RHIMeshPtr> vegetationMeshes;
		TVector<glm::mat4> vegetationModelMatrices;
		Math::AABB vegetationBounds;
		if (!profile.m_model->CollectRenderData(
			profile.m_meshIndex,
			vegetationMeshes,
			vegetationModelMatrices,
			vegetationBounds))
		{
			return EVegetationProxyBuildResult::NoRenderData;
		}

		TVector<MaterialPtr> vegetationMaterials;
		vegetationMaterials.Reserve(vegetationMeshes.Num());
		for (size_t meshIndex = 0u; meshIndex < vegetationMeshes.Num(); ++meshIndex)
		{
			MaterialPtr material = profile.m_material;
			if (!bUseMaterialOverride)
			{
				const size_t materialIndex = vegetationMeshes[meshIndex]->ResolveMaterialIndex(
					meshIndex,
					profile.m_modelMaterials.Num());
				material = materialIndex < profile.m_modelMaterials.Num() ?
					profile.m_modelMaterials[materialIndex] : MaterialPtr{};
			}
			if (!material || !material->IsReady())
			{
				return EVegetationProxyBuildResult::Pending;
			}
			vegetationMaterials.Add(std::move(material));
		}

		RHI::RHISceneViewProxy vegetationProxy;
		vegetationProxy.m_staticMeshEcs = LandscapeVegetationProxyId(
			componentIndex,
			chunkIndex,
			profileIndex);
		vegetationProxy.m_mobility = mobility;
		vegetationProxy.m_worldMatrix = ownerMatrix;
		vegetationProxy.m_frame = frame;
		vegetationProxy.m_bCastShadows =
			profile.m_shadowMode != ELandscapeVegetationShadowMode::None;
		vegetationProxy.m_lodPolicy.m_bEnabled = true;
		vegetationProxy.m_lodPolicy.m_minLod = profile.m_minLod;
		vegetationProxy.m_lodPolicy.m_maxLod = profile.m_maxLod;
		vegetationProxy.m_lodPolicy.m_screenCoverageThresholds =
			profile.m_screenCoverageThresholds;
		vegetationProxy.m_lodPolicy.m_maxCameraDistance = profile.m_cullDistance;

		RHI::RHIInstancedMeshGroup instanceGroup;
		instanceGroup.m_bCastShadows = vegetationProxy.m_bCastShadows;
		instanceGroup.m_maxShadowDistance = (std::min)(
			profile.m_cullDistance,
			profile.m_shadowMode == ELandscapeVegetationShadowMode::NearOnly ?
				profile.m_shadowDistance :
				(std::numeric_limits<float>::max)());
		instanceGroup.m_materials.Reserve(vegetationMeshes.Num());
		instanceGroup.m_sourceMaterialShaders.Reserve(vegetationMeshes.Num());
		instanceGroup.m_renderQueueTags.Reserve(vegetationMeshes.Num());
		instanceGroup.m_baseColorFactors.Reserve(vegetationMeshes.Num());
		instanceGroup.m_baseColorSamplers.Reserve(vegetationMeshes.Num());
		instanceGroup.m_alphaCutoffs.Reserve(vegetationMeshes.Num());
#if defined(__APPLE__)
		instanceGroup.m_materialTextureSamplers.Resize(vegetationMeshes.Num());
#endif
		auto* textureImporter = App::GetSubmodule<TextureImporter>();
		for (size_t meshIndex = 0u; meshIndex < vegetationMeshes.Num(); ++meshIndex)
		{
			MaterialPtr& material = vegetationMaterials[meshIndex];
			instanceGroup.m_sourceMaterialShaders.Add(material->GetShader());
			instanceGroup.m_materials.Add(material->GetOrAddRHI(
				vegetationMeshes[meshIndex]->m_vertexDescription));
			instanceGroup.m_renderQueueTags.Add(material->GetRenderState().GetTag());

			glm::vec4 baseColorFactor{ 1.0f };
			const glm::vec4* materialBaseColorFactor = nullptr;
			if (!material->GetUniformsVec4().Find(
				"material.baseColorFactor",
				materialBaseColorFactor))
			{
				material->GetUniformsVec4().Find("material.albedo", materialBaseColorFactor);
			}
			if (materialBaseColorFactor)
			{
				baseColorFactor = *materialBaseColorFactor;
			}
			instanceGroup.m_baseColorFactors.Add(baseColorFactor);

			float alphaCutoff = 0.5f;
			const float* materialAlphaCutoff = nullptr;
			if (material->GetUniformsFloat().Find(
				"material.alphaCutoff",
				materialAlphaCutoff) && materialAlphaCutoff)
			{
				alphaCutoff = *materialAlphaCutoff;
			}
			instanceGroup.m_alphaCutoffs.Add(alphaCutoff);

			uint32_t baseColorSampler = 0u;
			const TexturePtr* baseColorTexture = nullptr;
			if (!material->GetSamplers().Find("baseColorSampler", baseColorTexture))
			{
				material->GetSamplers().Find("albedoSampler", baseColorTexture);
			}
			if (textureImporter && baseColorTexture && *baseColorTexture)
			{
				baseColorSampler = static_cast<uint32_t>(
					textureImporter->GetTextureIndex((*baseColorTexture)->GetFileId()));
			}
			instanceGroup.m_baseColorSamplers.Add(baseColorSampler);
#if defined(__APPLE__)
			auto& requested = instanceGroup.m_materialTextureSamplers[meshIndex];
			requested.Insert(0u);
			if (textureImporter)
			{
				for (const auto& sampler : material->GetSamplers())
				{
					requested.Insert(sampler.m_second ? static_cast<uint32_t>(
						textureImporter->GetTextureIndex(sampler.m_second->GetFileId())) : 0u);
				}
			}
#endif
		}

		const uint32_t instanceCount = static_cast<uint32_t>(instances.m_transforms.Num());
		Math::AABB batchedVegetationBounds;
		for (const auto& localInstanceMatrix : instances.m_transforms)
		{
			const glm::mat4 instanceMatrix = ownerMatrix * localInstanceMatrix;
			Math::AABB instanceBounds = vegetationBounds;
			instanceBounds.Apply(instanceMatrix);
			batchedVegetationBounds.Extend(instanceBounds);
		}
		if (instances.m_transforms.IsEmpty() ||
			vegetationMeshes.IsEmpty() ||
			!batchedVegetationBounds.IsValid())
		{
			return EVegetationProxyBuildResult::NoRenderData;
		}
		instanceGroup.m_meshes = std::move(vegetationMeshes);
		instanceGroup.m_meshTransforms = std::move(vegetationModelMatrices);
		instanceGroup.m_instanceTransforms = std::move(instances.m_transforms);
		instanceGroup.m_instanceLodBiases = std::move(instances.m_lodBiases);
		instanceGroup.m_instanceCullDistanceScales =
			std::move(instances.m_cullDistanceScales);
		instanceGroup.m_instanceShadowDistanceScales =
			std::move(instances.m_shadowDistanceScales);

		vegetationProxy.m_worldAabb = batchedVegetationBounds;
		auto vegetationShadowCaster = RHI::RHIShadowCasterProxyPtr::Make();
		vegetationShadowCaster->m_staticMeshEcs = vegetationProxy.m_staticMeshEcs;
		vegetationShadowCaster->m_mobility = mobility;
		vegetationShadowCaster->m_skeletonOffset =
			(std::numeric_limits<uint32_t>::max)();
		vegetationShadowCaster->m_frame = vegetationProxy.m_frame;
		vegetationShadowCaster->m_lodPolicy = vegetationProxy.m_lodPolicy;
		vegetationShadowCaster->m_worldAabb = vegetationProxy.m_worldAabb;
		vegetationProxy.m_shadowCaster = vegetationProxy.m_bCastShadows ?
			vegetationShadowCaster : RHI::RHIShadowCasterProxyPtr{};
		vegetationProxy.m_instancedGroups.Add(std::move(instanceGroup));

		GetOctreeBounds(
			vegetationProxy.m_worldAabb,
			result.m_octreeCenter,
			result.m_octreeExtents);
		result.m_resource = RHI::RHISceneProxyResourcePtr::Make(
			std::move(vegetationProxy));
		result.m_profileIndex = profileIndex;
		result.m_instanceCount = instanceCount;
		result.m_revision = revision;
		result.m_mobility = mobility;
		return EVegetationProxyBuildResult::Success;
	}

	bool AreVegetationProfileSettingsEqual(
		const LandscapeVegetationProfile& lhs,
		const LandscapeVegetationProfile& rhs)
	{
		return lhs.m_modelFileId == rhs.m_modelFileId &&
			lhs.m_materialFileId == rhs.m_materialFileId &&
			lhs.m_meshIndex == rhs.m_meshIndex &&
			lhs.m_instancesPerChunk == rhs.m_instancesPerChunk &&
			lhs.m_residency == rhs.m_residency &&
			lhs.m_priority == rhs.m_priority &&
			lhs.m_minScale == rhs.m_minScale &&
			lhs.m_maxScale == rhs.m_maxScale &&
			lhs.m_groundOffset == rhs.m_groundOffset &&
			lhs.m_shadowMode == rhs.m_shadowMode &&
			lhs.m_shadowDistance == rhs.m_shadowDistance &&
			lhs.m_minLod == rhs.m_minLod &&
			lhs.m_maxLod == rhs.m_maxLod &&
			lhs.m_screenCoverageThresholds == rhs.m_screenCoverageThresholds &&
			lhs.m_cullDistance == rhs.m_cullDistance &&
			lhs.m_colliderRadius == rhs.m_colliderRadius &&
			lhs.m_colliderHeight == rhs.m_colliderHeight &&
			lhs.m_colliderOffsetY == rhs.m_colliderOffsetY;
	}

	uint64_t CalculateVegetationMaterialRenderMetadataRevision(
		const LandscapeVegetationProfile& profile)
	{
		size_t result = 1469598103934665603ull;
		if (profile.m_materialFileId)
		{
			HashCombine(
				result,
				profile.m_material,
				profile.m_material ? profile.m_material->GetRenderMetadataRevision() : 0ull);
		}
		else
		{
			HashCombine(result, profile.m_modelMaterials.Num());
			for (const auto& material : profile.m_modelMaterials)
			{
				HashCombine(
					result,
					material,
					material ? material->GetRenderMetadataRevision() : 0ull);
			}
		}
		return static_cast<uint64_t>(result);
	}

	void MarkChunksIntersectingStamp(
		LandscapeData& data,
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
					x * data.m_chunkSize - landscapeWidth * 0.5f,
					z * data.m_chunkSize - landscapeDepth * 0.5f);
				const glm::vec2 maximum = minimum + glm::vec2(data.m_chunkSize);
				const glm::vec2 closest = glm::clamp(center, minimum, maximum);
				if (glm::distance(center, closest) <= radius)
				{
					data.m_dirtyChunks.Insert(z * data.m_chunksX + x);
				}
			}
		}
	}

	void MarkChunksAffectedByStampChanges(
		LandscapeData& data,
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

	float SampleLandscapeChunkHeight(
		const LandscapeData& data,
		const LandscapeChunk& chunk,
		float x,
		float z)
	{
		const uint32_t resolution = chunk.m_heightResolution;
		const uint32_t row = resolution + 1u;
		if (resolution == 0u ||
			chunk.m_heightSamples.Num() < static_cast<size_t>(row) * row)
		{
			return 0.0f;
		}

		const float landscapeWidth = data.m_chunksX * data.m_chunkSize;
		const float landscapeDepth = data.m_chunksZ * data.m_chunkSize;
		const float originX = chunk.m_chunkX * data.m_chunkSize - landscapeWidth * 0.5f;
		const float originZ = chunk.m_chunkZ * data.m_chunkSize - landscapeDepth * 0.5f;
		const float sampleX = (std::clamp)(
			(x - originX) / data.m_chunkSize,
			0.0f,
			1.0f) * resolution;
		const float sampleZ = (std::clamp)(
			(z - originZ) / data.m_chunkSize,
			0.0f,
			1.0f) * resolution;
		const uint32_t x0 = static_cast<uint32_t>(std::floor(sampleX));
		const uint32_t z0 = static_cast<uint32_t>(std::floor(sampleZ));
		const uint32_t x1 = (std::min)(x0 + 1u, resolution);
		const uint32_t z1 = (std::min)(z0 + 1u, resolution);
		const float tx = sampleX - static_cast<float>(x0);
		const float tz = sampleZ - static_cast<float>(z0);
		const float a = glm::mix(
			chunk.m_heightSamples[static_cast<size_t>(z0) * row + x0],
			chunk.m_heightSamples[static_cast<size_t>(z0) * row + x1],
			tx);
		const float b = glm::mix(
			chunk.m_heightSamples[static_cast<size_t>(z1) * row + x0],
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
		auto* assetInfo = registry ?
			registry->GetAssetInfoPtr<LandscapeVegetationAssetInfoPtr>(
				data.m_vegetationAsset) : nullptr;
		if (!assetInfo)
		{
			SAILOR_LOG_ERROR(
				"LandscapeECS: vegetation asset %s is not registered.",
				data.m_vegetationAsset.ToString().c_str());
			return data.m_bVegetationAssetLoaded;
		}

		LandscapeVegetationAssetData loaded;
		std::string diagnostic;
		if (!loaded.Load(assetInfo->GetAssetFilepath(), diagnostic))
		{
			SAILOR_LOG_ERROR(
				"LandscapeECS: cannot load vegetation asset %s: %s",
				data.m_vegetationAsset.ToString().c_str(),
				diagnostic.c_str());
			return data.m_bVegetationAssetLoaded;
		}
		if (loaded.m_chunksX != data.m_chunksX ||
			loaded.m_chunksZ != data.m_chunksZ ||
			loaded.m_chunkSize != data.m_chunkSize)
		{
			SAILOR_LOG_ERROR(
				"LandscapeECS: vegetation asset %s targets %ux%u chunks of %.3fm, but the component uses %ux%u chunks of %.3fm.",
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
		SAILOR_LOG(
			"LandscapeECS: loaded %llu instances from vegetation asset %s.",
			static_cast<unsigned long long>(instanceCount),
			data.m_vegetationAsset.ToString().c_str());
		return true;
	}

	bool BuildProceduralVegetationAssetData(
		const LandscapeData& data,
		LandscapeVegetationAssetData& result,
		std::string& outDiagnostic)
	{
		const size_t expectedChunkCount =
			static_cast<size_t>(data.m_chunksX) * data.m_chunksZ;
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
			const float originX = chunk.m_chunkX * data.m_chunkSize -
				landscapeWidth * 0.5f;
			const float originZ = chunk.m_chunkZ * data.m_chunkSize -
				landscapeDepth * 0.5f;
			for (size_t profileIndex = 0u;
				profileIndex < data.m_vegetationProfiles.Num();
				++profileIndex)
			{
				const auto& profile = data.m_vegetationProfiles[profileIndex];
				if (!profile.m_modelFileId)
				{
					continue;
				}
				chunk.m_instances.Reserve(
					chunk.m_instances.Num() + profile.m_instancesPerChunk);
				for (uint32_t instanceIndex = 0u;
					instanceIndex < profile.m_instancesPerChunk;
					++instanceIndex)
				{
					const uint32_t randomSeed = data.m_seed ^
						static_cast<uint32_t>(chunk.m_chunkX * 92821u +
							chunk.m_chunkZ * 68917u +
							profileIndex * 4099u +
							instanceIndex * 131u);
					const float positionX = originX +
						Random01(randomSeed) * data.m_chunkSize;
					const float positionZ = originZ +
						Random01(randomSeed + 1u) * data.m_chunkSize;
					chunk.m_instances.Add(BuildProceduralVegetationInstance(
						data,
						chunk.m_chunkX,
						chunk.m_chunkZ,
						profileIndex,
						instanceIndex,
						SampleLandscapeChunkHeight(
							data,
							sourceChunk,
							positionX,
							positionZ)));
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
			SAILOR_LOG_ERROR(
				"LandscapeECS: Save Vegetation requires a vegetation asset reference.");
			return false;
		}

		auto* registry = App::GetSubmodule<AssetRegistry>();
		auto* assetInfo = registry ?
			registry->GetAssetInfoPtr<LandscapeVegetationAssetInfoPtr>(
				data.m_vegetationAsset) : nullptr;
		if (!assetInfo || !assetInfo->IsWritable())
		{
			SAILOR_LOG_ERROR(
				"LandscapeECS: vegetation asset %s is missing or read-only.",
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
			SAILOR_LOG_ERROR(
				"LandscapeECS: cannot prepare vegetation asset %s: %s",
				data.m_vegetationAsset.ToString().c_str(),
				diagnostic.c_str());
			return false;
		}
		if (!source->Save(assetInfo->GetAssetFilepath(), diagnostic))
		{
			SAILOR_LOG_ERROR(
				"LandscapeECS: cannot save vegetation asset %s: %s",
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
		SAILOR_LOG(
			"LandscapeECS: saved %llu instances to vegetation asset %s.",
			static_cast<unsigned long long>(savedInstanceCount),
			data.m_vegetationAsset.ToString().c_str());
		return true;
	}

	uint64_t CalculateGrassViewRevision(
		const LandscapeData& data,
		uint32_t instanceCapacity,
		const glm::mat4& inverseOwnerMatrix,
		const TVector<glm::vec3>& cameraPositions)
	{
		const float meanInstanceSpacing = data.m_chunkSize /
			std::sqrt(static_cast<float>((std::max)(instanceCapacity, 1u)));
		const double cellSize = static_cast<double>((std::clamp)(
			meanInstanceSpacing,
			0.5f,
			4.0f));
		auto quantize = [cellSize](float value)
			{
				if (!std::isfinite(value))
				{
					return int64_t(0);
				}
				const double coordinate = std::floor(static_cast<double>(value) / cellSize);
				return static_cast<int64_t>((std::clamp)(
					coordinate,
					static_cast<double>((std::numeric_limits<int64_t>::lowest)()),
					static_cast<double>((std::numeric_limits<int64_t>::max)())));
			};

		size_t result = 1469598103934665603ull;
		HashCombine(result, cameraPositions.Num());
		for (const glm::vec3& worldPosition : cameraPositions)
		{
			const glm::vec3 localPosition = glm::vec3(
				inverseOwnerMatrix * glm::vec4(worldPosition, 1.0f));
			HashCombine(
				result,
				quantize(localPosition.x),
				quantize(localPosition.y),
				quantize(localPosition.z));
		}
		return static_cast<uint64_t>(result) | 1ull;
	}

	LandscapeVegetationRenderInstances BuildGrassInstanceTransforms(
		const LandscapeData& data,
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
		if (const auto* authored = GetAuthoredVegetationChunk(
				data,
				chunk.m_chunkX,
				chunk.m_chunkZ))
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
			for (uint32_t instanceIndex = 0u;
				instanceIndex < profile.m_instancesPerChunk;
				++instanceIndex)
			{
				const uint32_t randomSeed = data.m_seed ^
					static_cast<uint32_t>(chunk.m_chunkX * 92821u +
						chunk.m_chunkZ * 68917u +
						profileIndex * 4099u +
						instanceIndex * 131u);
				const float positionX = originX +
					Random01(randomSeed) * data.m_chunkSize;
				const float positionZ = originZ +
					Random01(randomSeed + 1u) * data.m_chunkSize;
				InstancePlacement placement;
				placement.m_instance = BuildProceduralVegetationInstance(
					data,
					chunk.m_chunkX,
					chunk.m_chunkZ,
					profileIndex,
					instanceIndex,
					SampleLandscapeChunkHeight(
						data,
						chunk,
						positionX,
						positionZ));
				placement.m_instanceIndex = instanceIndex;
				placements.Add(std::move(placement));
			}
		}

		instanceCount = (std::min)(
			instanceCount,
			static_cast<uint32_t>(placements.Num()));
		result.m_transforms.Reserve(instanceCount);
		result.m_lodBiases.Reserve(instanceCount);
		result.m_cullDistanceScales.Reserve(instanceCount);
		result.m_shadowDistanceScales.Reserve(instanceCount);
		const bool bPartialSelection =
			instanceCount < placements.Num() &&
			!cameraPositions.IsEmpty();
		if (bPartialSelection)
		{
			for (auto& placement : placements)
			{
				placement.m_cameraDistanceSquared =
					(std::numeric_limits<float>::infinity)();
				const glm::vec3 localPosition = glm::vec3(
					placement.m_instance.m_transform[3]);
				const glm::vec3 worldPosition = glm::vec3(
					ownerMatrix * glm::vec4(localPosition, 1.0f));
				for (const glm::vec3& cameraPosition : cameraPositions)
				{
					const glm::vec3 delta = worldPosition - cameraPosition;
					placement.m_cameraDistanceSquared = (std::min)(
						placement.m_cameraDistanceSquared,
						glm::dot(delta, delta));
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
			std::nth_element(
				placements.begin(),
				placements.begin() + instanceCount,
				placements.end(),
				compareDistance);
			placements.Resize(instanceCount);
			placements.Sort([](const InstancePlacement& lhs, const InstancePlacement& rhs)
				{
					return lhs.m_instanceIndex < rhs.m_instanceIndex;
				});
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

void LandscapeData::SetSettings(uint32_t chunksX, uint32_t chunksZ,
	float chunkSize, uint32_t chunkResolution, float heightScale,
	float noiseScale, uint32_t seed, float textureTiling)
{
	const uint32_t normalizedChunksX = (std::clamp)(chunksX, 1u, 64u);
	const uint32_t normalizedChunksZ = (std::clamp)(chunksZ, 1u, 64u);
	const float normalizedChunkSize = (std::max)(chunkSize, 1.0f);
	const uint32_t normalizedChunkResolution = (std::clamp)(chunkResolution, 2u, 128u);
	const float normalizedHeightScale = (std::max)(heightScale, 0.0f);
	const float normalizedNoiseScale = (std::max)(noiseScale, 0.0001f);
	const float normalizedTextureTiling = (std::max)(textureTiling, 0.001f);
	if (m_chunksX == normalizedChunksX &&
		m_chunksZ == normalizedChunksZ &&
		m_chunkSize == normalizedChunkSize &&
		m_chunkResolution == normalizedChunkResolution &&
		m_heightScale == normalizedHeightScale &&
		m_noiseScale == normalizedNoiseScale &&
		m_seed == seed &&
		m_textureTiling == normalizedTextureTiling)
	{
		return;
	}

	m_chunksX = normalizedChunksX;
	m_chunksZ = normalizedChunksZ;
	m_chunkSize = normalizedChunkSize;
	m_chunkResolution = normalizedChunkResolution;
	m_heightScale = normalizedHeightScale;
	m_noiseScale = normalizedNoiseScale;
	m_seed = seed;
	m_textureTiling = normalizedTextureTiling;
	RequestFullRebuild();
}

void LandscapeData::SetMaterial(const MaterialPtr& material)
{
	if (m_material == material)
	{
		return;
	}
	m_material = material;
	m_runtimeMaterial.Clear();
	m_cachedSourceMaterialContentRevision = 0ull;
	m_cachedSourceMaterialRenderMetadataRevision = 0ull;
	RequestFullRebuild();
}

void LandscapeData::SetLodSettings(
	const TVector<float>& distances,
	float skirtDepth)
{
	TVector<float> normalizedDistances;
	normalizedDistances.Reserve((std::min)(distances.Num(), size_t(7u)));
	for (float distance : distances)
	{
		if (normalizedDistances.Num() >= 7u)
		{
			break;
		}
		if (std::isfinite(distance))
		{
			normalizedDistances.Add((std::max)(distance, 1.0f));
		}
	}
	std::sort(normalizedDistances.begin(), normalizedDistances.end());
	const float normalizedSkirtDepth = std::isfinite(skirtDepth) ?
		(std::clamp)(skirtDepth, 0.0f, 64.0f) : 2.0f;
	if (m_lodDistances == normalizedDistances &&
		m_lodSkirtDepth == normalizedSkirtDepth)
	{
		return;
	}
	m_lodDistances = std::move(normalizedDistances);
	m_lodSkirtDepth = normalizedSkirtDepth;
	RequestFullRebuild();
}

void LandscapeData::SetGrassResidencyHysteresis(
	float grassResidencyHysteresis)
{
	const float normalizedHysteresis = std::isfinite(grassResidencyHysteresis) ?
		(std::clamp)(grassResidencyHysteresis, 0.0f, 512.0f) : 12.0f;
	if (m_grassResidencyHysteresis == normalizedHysteresis)
	{
		return;
	}
	m_grassResidencyHysteresis = normalizedHysteresis;
}

void LandscapeData::SetLayerTextures(const TVector<FileId>& textures)
{
	TVector<FileId> normalized = textures;
	if (normalized.Num() > 4u) normalized.Resize(4u);
	if (m_layerTextures == normalized)
	{
		return;
	}
	m_layerTextures = std::move(normalized);
	m_runtimeMaterial.Clear();
	m_cachedSourceMaterialContentRevision = 0ull;
	m_cachedSourceMaterialRenderMetadataRevision = 0ull;
	RequestFullRebuild();
}

void LandscapeData::SetImportMaps(const FileId& heightmapTexture,
	const TVector<FileId>& materialMasks)
{
	TVector<FileId> normalizedMasks = materialMasks;
	if (normalizedMasks.Num() > 4u) normalizedMasks.Resize(4u);
	if (m_heightmapTexture == heightmapTexture && m_materialMasks == normalizedMasks)
	{
		return;
	}
	m_heightmapTexture = heightmapTexture;
	m_materialMasks = std::move(normalizedMasks);
	RequestFullRebuild();
}

void LandscapeData::SetAuthoredStamps(const TVector<float>& sculptStamps,
	const TVector<float>& paintStamps)
{
	if (m_sculptStamps == sculptStamps && m_paintStamps == paintStamps)
	{
		return;
	}

	if (!m_bRebuildAllChunks &&
		m_chunks.Num() == static_cast<size_t>(m_chunksX) * m_chunksZ)
	{
		const float normalSampleMargin = m_chunkSize /
			static_cast<float>((std::max)(m_chunkResolution, 1u));
		MarkChunksAffectedByStampChanges(
			*this,
			m_sculptStamps,
			sculptStamps,
			normalSampleMargin);
		MarkChunksAffectedByStampChanges(
			*this,
			m_paintStamps,
			paintStamps,
			0.0f);
	}
	else
	{
		m_bRebuildAllChunks = true;
	}
	m_sculptStamps = sculptStamps;
	m_paintStamps = paintStamps;
	MarkDirty();
}

void LandscapeData::SetVegetationAsset(const FileId& vegetationAsset)
{
	if (m_vegetationAsset == vegetationAsset)
	{
		return;
	}
	m_vegetationAsset = vegetationAsset;
	m_vegetationAssetData = {};
	m_bVegetationAssetLoaded = false;
	RequestVegetationAssetReload();
}

void LandscapeData::RequestVegetationAssetReload()
{
	m_bReloadVegetationAsset = static_cast<bool>(m_vegetationAsset);
	RequestFullRebuild();
}

void LandscapeData::RequestSaveVegetation()
{
	m_bSaveVegetationRequested = true;
	MarkDirty();
}

void LandscapeData::RequestFullRebuild()
{
	m_bRebuildAllChunks = true;
	m_dirtyChunks.Clear();
	MarkDirty();
}

void LandscapeData::SetVegetationProfiles(
	const TVector<FileId>& models,
	const TVector<FileId>& materials,
	const TVector<float>& meshIndex,
	const TVector<float>& instancesPerChunk,
	const TVector<float>& residency,
	const TVector<float>& priority,
	const TVector<float>& minScale,
	const TVector<float>& maxScale,
	const TVector<float>& groundOffset,
	const TVector<float>& shadowMode,
	const TVector<float>& shadowDistance,
	const TVector<float>& minLod,
	const TVector<float>& maxLod,
	const TVector<float>& lod1ScreenCoverage,
	const TVector<float>& lod2ScreenCoverage,
	const TVector<float>& cullDistance,
	const TVector<float>& colliderRadius,
	const TVector<float>& colliderHeight,
	const TVector<float>& colliderOffsetY)
{
	TVector<LandscapeVegetationProfile> profiles;
	const size_t numProfiles = models.Num();
	profiles.Reserve(numProfiles);
	for (size_t index = 0u; index < numProfiles; ++index)
	{
		LandscapeVegetationProfile profile;
		profile.m_modelFileId = models[index];
		profile.m_materialFileId = index < materials.Num() ? materials[index] : FileId{};
		profile.m_meshIndex = static_cast<int32_t>((std::clamp)(
			GetProfileValue(meshIndex, index, -1.0f), -1.0f, 65535.0f));
		profile.m_instancesPerChunk = static_cast<uint32_t>((std::clamp)(
			GetProfileValue(instancesPerChunk, index, 0.0f), 0.0f, 2048.0f));
		profile.m_residency = static_cast<ELandscapeVegetationResidency>(
			static_cast<uint32_t>((std::clamp)(
				GetProfileValue(residency, index, 0.0f), 0.0f, 1.0f)));
		profile.m_priority = (std::clamp)(
			GetProfileValue(priority, index, 1.0f), 0.0f, 100.0f);
		profile.m_minScale = (std::max)(GetProfileValue(minScale, index, 0.75f), 0.01f);
		profile.m_maxScale = (std::max)(GetProfileValue(maxScale, index, 1.25f), profile.m_minScale);
		profile.m_groundOffset = GetProfileValue(groundOffset, index, 0.0f);
		profile.m_shadowMode = static_cast<ELandscapeVegetationShadowMode>(
			static_cast<uint32_t>((std::clamp)(GetProfileValue(shadowMode, index, 1.0f), 0.0f, 2.0f)));
		profile.m_shadowDistance = (std::max)(GetProfileValue(shadowDistance, index, 35.0f), 0.1f);
		profile.m_minLod = static_cast<uint32_t>((std::clamp)(
			GetProfileValue(minLod, index, 0.0f), 0.0f, 15.0f));
		profile.m_maxLod = static_cast<uint32_t>((std::clamp)(
			GetProfileValue(maxLod, index, 2.0f),
			static_cast<float>(profile.m_minLod), 15.0f));
		profile.m_screenCoverageThresholds = {
			(std::clamp)(GetProfileValue(lod1ScreenCoverage, index, 0.25f), 0.0f, 1.0f),
			(std::clamp)(GetProfileValue(lod2ScreenCoverage, index, 0.05f), 0.0f, 1.0f)
		};
		std::sort(profile.m_screenCoverageThresholds.begin(),
			profile.m_screenCoverageThresholds.end(), std::greater<float>());
		profile.m_cullDistance = (std::max)(GetProfileValue(cullDistance, index, 120.0f), 0.1f);
		profile.m_colliderRadius = (std::max)(GetProfileValue(colliderRadius, index, 0.0f), 0.0f);
		profile.m_colliderHeight = (std::max)(GetProfileValue(colliderHeight, index, 2.0f),
			profile.m_colliderRadius * 2.0f);
		profile.m_colliderOffsetY = GetProfileValue(colliderOffsetY, index, 1.0f);
		if (profile.m_residency == ELandscapeVegetationResidency::Grass)
		{
			profile.m_colliderRadius = 0.0f;
		}
		profiles.Add(std::move(profile));
	}

	bool bSettingsChanged = profiles.Num() != m_vegetationProfiles.Num();
	for (size_t index = 0u; !bSettingsChanged && index < profiles.Num(); ++index)
	{
		bSettingsChanged = !AreVegetationProfileSettingsEqual(
			profiles[index],
			m_vegetationProfiles[index]);
	}
	if (!bSettingsChanged)
	{
		return;
	}

	for (size_t index = 0u; index < profiles.Num() && index < m_vegetationProfiles.Num(); ++index)
	{
		auto& profile = profiles[index];
		const auto& previous = m_vegetationProfiles[index];
		if (profile.m_modelFileId == previous.m_modelFileId)
		{
			profile.m_model = previous.m_model;
			profile.m_modelMaterials = previous.m_modelMaterials;
			profile.m_bModelMaterialsRequested = previous.m_bModelMaterialsRequested;
		}
		if (profile.m_materialFileId == previous.m_materialFileId)
		{
			profile.m_material = previous.m_material;
			profile.m_cachedMaterialRenderMetadataRevision =
				previous.m_cachedMaterialRenderMetadataRevision;
		}
	}
	m_vegetationProfiles = std::move(profiles);
	RequestFullRebuild();
}

void LandscapeECS::BeginPlay()
{
	m_rhiScene = RHI::RHIScenePtr::Make();
	PublishSceneVersion();
}

void LandscapeECS::OnComponentUnregistered(size_t, LandscapeData& component)
{
	DestroyPhysicsBodies(component);
	component.m_chunks.Clear();
	component.m_runtimeMaterial.Clear();
	++m_shadowCastersRevision;
	PublishSceneVersion();
}

void LandscapeECS::DestroyPhysicsBodies(LandscapeData& component)
{
	auto* physics = GetWorld() ? GetWorld()->GetECS<PhysicsECS>() : nullptr;
	if (physics)
	{
		for (uint32_t bodyId : component.m_physicsBodies)
		{
			physics->DestroyExternalBody(bodyId);
		}
	}
	component.m_physicsBodies.Clear();
	for (auto& chunk : component.m_chunks)
	{
		chunk.m_physicsBodies.Clear();
	}
}

void LandscapeECS::DestroyChunkPhysicsBodies(
	LandscapeData& component,
	LandscapeChunk& chunk)
{
	auto* physics = GetWorld() ? GetWorld()->GetECS<PhysicsECS>() : nullptr;
	for (uint32_t bodyId : chunk.m_physicsBodies)
	{
		if (physics)
		{
			physics->DestroyExternalBody(bodyId);
		}
		component.m_physicsBodies.Remove(bodyId);
	}
	chunk.m_physicsBodies.Clear();
}

Tasks::ITaskPtr LandscapeECS::Tick(float)
{
	SAILOR_PROFILE_FUNCTION();
	bool bSceneChanged = false;
	const auto* cameraEcs = GetWorld() ? GetWorld()->GetECS<CameraECS>() : nullptr;
	const TVector<Math::Transform> noCameraTransforms;
	const TVector<CameraData> noCameras;
	const auto& cameraTransforms = cameraEcs ?
		cameraEcs->GetActiveCameraTransforms() : noCameraTransforms;
	const auto& cameras = cameraEcs ?
		cameraEcs->GetActiveCameras() : noCameras;
	for (size_t componentIndex = 0; componentIndex < m_components.Num(); ++componentIndex)
	{
		if (!IsComponentRegistered(componentIndex))
		{
			continue;
		}

		auto& data = m_components[componentIndex];
		GameObjectPtr owner = const_cast<ObjectPtr&>(data.GetOwner()).StaticCast<GameObject>();
		const bool transformChanged = owner && owner->GetTransformComponent().GetFrameLastChange() > data.GetFrameLastChange();
		const bool bTerrainMaterialReady = data.m_material && data.m_material->IsReady();
		if (data.m_runtimeMaterial)
		{
			// Publishing is fence-gated. Calling IsReady every tick advances the
			// runtime material only after its cloned bindings are upload-complete.
			data.m_runtimeMaterial->IsReady();
		}
		const bool bTerrainMaterialContentRevisionChanged =
			data.m_runtimeMaterial && data.m_material &&
			data.m_cachedSourceMaterialContentRevision !=
				data.m_material->GetContentRevision();
		const bool bTerrainMaterialRenderMetadataRevisionChanged =
			data.m_runtimeMaterial && data.m_material &&
			data.m_cachedSourceMaterialRenderMetadataRevision !=
				data.m_material->GetRenderMetadataRevision();
		bool bVegetationMaterialRenderMetadataRevisionChanged = false;
		for (const auto& profile : data.m_vegetationProfiles)
		{
			if (profile.m_material)
			{
				profile.m_material->IsReady();
			}
			for (const auto& material : profile.m_modelMaterials)
			{
				if (material)
				{
					material->IsReady();
				}
			}
			bVegetationMaterialRenderMetadataRevisionChanged |=
				profile.m_cachedMaterialRenderMetadataRevision !=
				CalculateVegetationMaterialRenderMetadataRevision(profile);
		}
		if (bTerrainMaterialRenderMetadataRevisionChanged)
		{
			data.m_runtimeMaterial.Clear();
		}
		if (bTerrainMaterialRenderMetadataRevisionChanged ||
			bVegetationMaterialRenderMetadataRevisionChanged)
		{
			data.RequestFullRebuild();
		}
		else if (bTerrainMaterialContentRevisionChanged)
		{
			if (bTerrainMaterialReady)
			{
				// Landscape keeps its layer samplers on a private material instance.
				// Copy only source uniform values and version its bindings in place;
				// chunk meshes, physics, vegetation and scene records remain shared.
				data.m_runtimeMaterial->SynchronizeUniformValues(*data.m_material);
				data.m_cachedSourceMaterialContentRevision =
					data.m_material->GetContentRevision();
			}
			else
			{
				data.MarkDirty();
			}
		}
		if (!data.IsDirty() && !transformChanged)
		{
			continue;
		}
		if (!owner || !bTerrainMaterialReady)
		{
			data.MarkDirty();
			continue;
		}

		if (!data.m_runtimeMaterial)
		{
			data.m_runtimeMaterial = Material::CreateInstance(GetWorld(), data.m_material);
			static const char* LayerNames[] = { "layer0Sampler", "layer1Sampler", "layer2Sampler", "layer3Sampler" };
			auto* textureImporter = App::GetSubmodule<TextureImporter>();
			for (size_t layer = 0; textureImporter && layer < data.m_layerTextures.Num() && layer < 4u; ++layer)
			{
				TexturePtr texture;
				if (data.m_layerTextures[layer] && textureImporter->LoadTexture_Immediate(data.m_layerTextures[layer], texture) && texture)
				{
					data.m_runtimeMaterial->SetSampler(LayerNames[layer], texture);
				}
			}
			data.m_runtimeMaterial->UpdateRHIResourceAndUniforms();
			data.m_cachedSourceMaterialContentRevision =
				data.m_material->GetContentRevision();
			data.m_cachedSourceMaterialRenderMetadataRevision =
				data.m_material->GetRenderMetadataRevision();
		}

		auto* modelImporter = App::GetSubmodule<ModelImporter>();
		auto* materialImporter = App::GetSubmodule<MaterialImporter>();
		for (auto& profile : data.m_vegetationProfiles)
		{
			if (!profile.m_model && modelImporter && profile.m_modelFileId)
			{
				modelImporter->LoadModel_Immediate(profile.m_modelFileId, profile.m_model);
			}
			if (profile.m_materialFileId && !profile.m_material && materialImporter)
			{
				materialImporter->LoadMaterial_Immediate(profile.m_materialFileId, profile.m_material);
			}
			else if (!profile.m_materialFileId && !profile.m_bModelMaterialsRequested && modelImporter)
			{
				modelImporter->LoadDefaultMaterials(profile.m_modelFileId, profile.m_modelMaterials);
				profile.m_bModelMaterialsRequested = true;
			}
		}
		TryLoadVegetationAsset(data);

		const size_t numLandscapeChunks = static_cast<size_t>(data.m_chunksX) * data.m_chunksZ;
		const bool bRebuildAllChunks = data.m_bRebuildAllChunks ||
			transformChanged || data.m_chunks.Num() != numLandscapeChunks;
		TVector<uint32_t> chunksToBuild;
		if (bRebuildAllChunks)
		{
			chunksToBuild.Resize(numLandscapeChunks);
			for (uint32_t chunkIndex = 0u; chunkIndex < chunksToBuild.Num(); ++chunkIndex)
			{
				chunksToBuild[chunkIndex] = chunkIndex;
			}
		}
		else
		{
			chunksToBuild = data.m_dirtyChunks.ToVector();
			size_t validChunkWriteIndex = 0u;
			for (const uint32_t chunkIndex : chunksToBuild)
			{
				if (chunkIndex < numLandscapeChunks)
				{
					chunksToBuild[validChunkWriteIndex++] = chunkIndex;
				}
			}
			chunksToBuild.Resize(validChunkWriteIndex);
			chunksToBuild.Sort();
		}
		if (chunksToBuild.IsEmpty())
		{
			TrySaveVegetationAsset(data);
			data.m_bIsDirty = false;
			data.SetLastChange(owner->GetTransformComponent().GetFrameLastChange());
			continue;
		}

		LandscapeCpuTexture heightmap;
		if (data.m_heightmapTexture && !DecodeCpuTexture(data.m_heightmapTexture, heightmap))
		{
			SAILOR_LOG_ERROR("LandscapeECS: failed to decode heightmap %s; procedural height will be used.",
				data.m_heightmapTexture.ToString().c_str());
		}
		TVector<LandscapeCpuTexture> materialMasks;
		materialMasks.Reserve(data.m_materialMasks.Num());
		for (const FileId& maskFileId : data.m_materialMasks)
		{
			LandscapeCpuTexture mask;
			if (!DecodeCpuTexture(maskFileId, mask) && maskFileId)
			{
				SAILOR_LOG_ERROR("LandscapeECS: failed to decode material mask %s.",
					maskFileId.ToString().c_str());
			}
			materialMasks.Add(std::move(mask));
		}

		TVector<Tasks::TaskPtr<LandscapeChunkCpuData>> tasks;
		tasks.Reserve(chunksToBuild.Num());
		for (uint32_t chunkIndex : chunksToBuild)
		{
			const uint32_t x = chunkIndex % data.m_chunksX;
			const uint32_t z = chunkIndex / data.m_chunksX;
			auto task = Tasks::CreateTask<LandscapeChunkCpuData>(
				"LandscapeECS:Build Chunk", [&data, &heightmap, &materialMasks, x, z]()
				{
					return BuildChunk(data, heightmap, materialMasks, x, z);
				}, EThreadType::Worker);
			task->Run();
			tasks.Add(task);
		}

		if (bRebuildAllChunks)
		{
			DestroyPhysicsBodies(data);
			data.m_chunks.Clear();
			data.m_chunks.Resize(numLandscapeChunks);
		}
		size_t vegetationRenderProxies = 0u;
		size_t vegetationProfilesNotReady = 0u;
		size_t vegetationProfilesWithoutRenderData = 0u;
		bool bVegetationResourcesPending = false;
		const glm::mat4 ownerMatrix = owner->GetTransformComponent().GetCachedWorldMatrix();
		const Math::Transform ownerTransform = Math::Transform::FromMatrix(ownerMatrix);
		auto* physics = GetWorld()->GetECS<PhysicsECS>();
		for (size_t taskIndex = 0u; taskIndex < tasks.Num(); ++taskIndex)
		{
			const uint32_t chunkIndex = chunksToBuild[taskIndex];
			tasks[taskIndex]->Wait();
			auto& cpu = tasks[taskIndex]->m_result;
			if (!bRebuildAllChunks)
			{
				DestroyChunkPhysicsBodies(data, data.m_chunks[chunkIndex]);
			}
			LandscapeChunk chunk;
			chunk.m_chunkX = cpu.m_chunkX;
			chunk.m_chunkZ = cpu.m_chunkZ;
			chunk.m_heightResolution = data.m_chunkResolution;
			const size_t terrainVertexCount =
				static_cast<size_t>(data.m_chunkResolution + 1u) *
				(data.m_chunkResolution + 1u);
			chunk.m_heightSamples.Resize(terrainVertexCount);
			TVector<glm::vec3> collisionVertices;
			collisionVertices.Resize(terrainVertexCount);
			for (size_t vertexIndex = 0u; vertexIndex < terrainVertexCount; ++vertexIndex)
			{
				collisionVertices[vertexIndex] = cpu.m_vertices[vertexIndex].m_position;
				chunk.m_heightSamples[vertexIndex] = cpu.m_vertices[vertexIndex].m_position.y;
			}
			uint32_t physicsBodyId = RigidBodyData::InvalidBodyId;
			if (physics && physics->CreateStaticTriangleMesh(owner->GetInstanceId(),
				collisionVertices, cpu.m_collisionIndices, glm::vec3(ownerTransform.m_position),
				ownerTransform.m_rotation, glm::vec3(ownerTransform.m_scale), physicsBodyId))
			{
				data.m_physicsBodies.Add(physicsBodyId);
				chunk.m_physicsBodies.Add(physicsBodyId);
			}
			TVector<Physics::CollisionShapeDesc> vegetationCollisionShapes;
			for (const auto& placement : cpu.m_vegetation)
			{
				if (placement.m_profileIndex >= data.m_vegetationProfiles.Num())
				{
					continue;
				}
				const auto& profile = data.m_vegetationProfiles[placement.m_profileIndex];
				if (profile.m_colliderRadius <= 0.0f)
				{
					continue;
				}
				const Math::Transform instanceTransform =
					Math::Transform::FromMatrix(placement.m_transform);
				const float instanceScale = (std::max)({
					std::abs(instanceTransform.m_scale.x),
					std::abs(instanceTransform.m_scale.y),
					std::abs(instanceTransform.m_scale.z)
				});
				Physics::CollisionShapeDesc shape;
				shape.m_type = Physics::ECollisionShapeType::Capsule;
				shape.m_center = glm::vec3(instanceTransform.TransformPosition(
					glm::vec4(0.0f, profile.m_colliderOffsetY, 0.0f, 1.0f)));
				shape.m_rotation = instanceTransform.m_rotation;
				shape.m_radius = profile.m_colliderRadius * instanceScale;
				shape.m_height = profile.m_colliderHeight * instanceScale;
				vegetationCollisionShapes.Add(std::move(shape));
			}
			if (physics && !vegetationCollisionShapes.IsEmpty() &&
				physics->CreateStaticCompound(owner->GetInstanceId(), vegetationCollisionShapes,
					glm::vec3(ownerTransform.m_position), ownerTransform.m_rotation,
					glm::vec3(ownerTransform.m_scale), physicsBodyId))
			{
				data.m_physicsBodies.Add(physicsBodyId);
				chunk.m_physicsBodies.Add(physicsBodyId);
			}
			auto mesh = RHI::Renderer::GetDriver()->CreateMesh();
			mesh->m_vertexDescription = RHI::Renderer::GetDriver()->GetOrAddVertexDescription<RHI::VertexP3N3T3B3UV2C4>();
			mesh->m_bounds = cpu.m_localBounds;
			mesh->m_materialIndex = 0u;
			mesh->m_indexCount = cpu.m_lodIndexCounts.IsEmpty() ?
				static_cast<uint32_t>(cpu.m_indices.Num()) : cpu.m_lodIndexCounts[0];
			mesh->m_firstIndex = 0u;
			mesh->m_vertexOffset = 0u;
			RHI::Renderer::GetDriver()->UpdateMesh(mesh,
				cpu.m_vertices.GetData(), cpu.m_vertices.Num() * sizeof(RHI::VertexP3N3T3B3UV2C4),
				cpu.m_indices.GetData(), cpu.m_indices.Num() * sizeof(uint32_t));
			for (size_t lodIndex = 1u; lodIndex < cpu.m_lodIndexCounts.Num(); ++lodIndex)
			{
				auto lodMesh = RHI::Renderer::GetDriver()->CreateMesh();
				lodMesh->m_vertexDescription = mesh->m_vertexDescription;
				lodMesh->m_vertexBuffer = mesh->m_vertexBuffer;
				lodMesh->m_indexBuffer = mesh->m_indexBuffer;
				lodMesh->m_bounds = mesh->m_bounds;
				lodMesh->m_materialIndex = mesh->m_materialIndex;
				lodMesh->m_bakedVolumeScale = mesh->m_bakedVolumeScale;
				lodMesh->m_indexCount = cpu.m_lodIndexCounts[lodIndex];
				lodMesh->m_firstIndex = cpu.m_lodFirstIndices[lodIndex];
				lodMesh->m_vertexOffset = 0u;
				mesh->m_lods.Add(std::move(lodMesh));
			}

			RHI::RHISceneViewProxy proxy;
			proxy.m_staticMeshEcs = LandscapeProxyId(componentIndex, chunkIndex);
			proxy.m_worldMatrix = ownerMatrix;
			proxy.m_worldAabb = cpu.m_localBounds;
			proxy.m_worldAabb.Apply(ownerMatrix);
			proxy.m_frame = GetWorld()->GetCurrentFrame();
			proxy.m_bCastShadows = true;
			proxy.m_lodPolicy.m_bEnabled = !mesh->m_lods.IsEmpty();
			proxy.m_lodPolicy.m_minLod = 0u;
			proxy.m_lodPolicy.m_maxLod = mesh->GetNumLods() - 1u;
			proxy.m_lodPolicy.m_cameraDistanceThresholds = data.m_lodDistances;
			if (proxy.m_lodPolicy.m_cameraDistanceThresholds.Num() >= mesh->GetNumLods())
			{
				proxy.m_lodPolicy.m_cameraDistanceThresholds.Resize(mesh->GetNumLods() - 1u);
			}
			proxy.m_meshes.Add(mesh);
			proxy.m_meshModelMatrices.Add(ownerMatrix);
			proxy.m_overrideMaterials.Add(data.m_runtimeMaterial->GetOrAddRHI(mesh->m_vertexDescription));
			proxy.m_renderQueueTags.Add(data.m_runtimeMaterial->GetRenderState().GetTag());
			AppendDepthMaterialMetadata(proxy, data.m_runtimeMaterial);
			auto shadowCaster = RHI::RHIShadowCasterProxyPtr::Make();
			shadowCaster->m_staticMeshEcs = proxy.m_staticMeshEcs;
			shadowCaster->m_lodPolicy = proxy.m_lodPolicy;
			shadowCaster->m_skeletonOffset = (std::numeric_limits<uint32_t>::max)();
			shadowCaster->m_frame = proxy.m_frame;
			AppendShadowMesh(*shadowCaster, mesh, ownerMatrix, data.m_runtimeMaterial,
				(std::numeric_limits<float>::max)());

			shadowCaster->m_worldAabb = proxy.m_worldAabb;
			proxy.m_shadowCaster = shadowCaster->m_meshes.IsEmpty() ?
				RHI::RHIShadowCasterProxyPtr{} : shadowCaster;
#if defined(__APPLE__)
			auto* textureImporter = App::GetSubmodule<TextureImporter>();
			proxy.m_materialTextureSamplers.Resize(1u);
			proxy.m_materialTextureSamplers[0].Insert(0u);
			if (textureImporter)
			{
				for (const auto& sampler : data.m_runtimeMaterial->GetSamplers())
				{
					proxy.m_materialTextureSamplers[0].Insert(sampler.m_second ?
						static_cast<uint32_t>(textureImporter->GetTextureIndex(sampler.m_second->GetFileId())) : 0u);
				}
			}
#endif
			GetOctreeBounds(proxy.m_worldAabb, chunk.m_octreeCenter, chunk.m_octreeExtents);

			for (size_t profileIndex = 0u; profileIndex < data.m_vegetationProfiles.Num(); ++profileIndex)
			{
				auto& profile = data.m_vegetationProfiles[profileIndex];
				if (profile.m_residency == ELandscapeVegetationResidency::Grass)
				{
					continue;
				}

				LandscapeVegetationRenderInstances instances;
				instances.m_transforms.Reserve(profile.m_instancesPerChunk);
				instances.m_lodBiases.Reserve(profile.m_instancesPerChunk);
				instances.m_cullDistanceScales.Reserve(profile.m_instancesPerChunk);
				instances.m_shadowDistanceScales.Reserve(profile.m_instancesPerChunk);
				for (const auto& placement : cpu.m_vegetation)
				{
					if (placement.m_profileIndex != profileIndex)
					{
						continue;
					}
					AppendRenderInstance(placement, instances);
				}

				LandscapeVegetationRenderProxy vegetationRenderProxy;
				const auto buildResult = BuildLandscapeVegetationProxy(
					componentIndex,
					chunkIndex,
					profileIndex,
					profile,
					ownerMatrix,
					proxy.m_frame,
					std::move(instances),
					EMobilityType::Static,
					data.m_buildRevision + 1u,
					vegetationRenderProxy);
				if (buildResult == EVegetationProxyBuildResult::Pending)
				{
					++vegetationProfilesNotReady;
					bVegetationResourcesPending = true;
					continue;
				}
				if (buildResult == EVegetationProxyBuildResult::NoRenderData)
				{
					++vegetationProfilesWithoutRenderData;
					continue;
				}
				chunk.m_vegetationProxies.Add(std::move(vegetationRenderProxy));
				++vegetationRenderProxies;
			}
			chunk.m_resource = RHI::RHISceneProxyResourcePtr::Make(std::move(proxy));
			chunk.m_buildRevision = ++data.m_buildRevision;
			data.m_chunks[chunkIndex] = std::move(chunk);
		}
		// Model and material import tasks complete before their GPU upload fences do.
		// Keep the component dirty while valid vegetation resources are pending so
		// the next frame can populate the vegetation proxies after those fences signal.
		data.m_dirtyChunks.Clear();
		data.m_bRebuildAllChunks = bVegetationResourcesPending;
		data.m_bIsDirty = bVegetationResourcesPending;
		for (auto& profile : data.m_vegetationProfiles)
		{
			profile.m_cachedMaterialRenderMetadataRevision =
				CalculateVegetationMaterialRenderMetadataRevision(profile);
		}
		data.SetLastChange(owner->GetTransformComponent().GetFrameLastChange());
		++m_shadowCastersRevision;
		bSceneChanged = true;
		size_t vegetationPerChunk = 0u;
		for (const auto& profile : data.m_vegetationProfiles)
		{
			vegetationPerChunk += profile.m_instancesPerChunk;
		}
		SAILOR_LOG("LandscapeECS: rebuilt %zu of %zu chunks with %zu collision bodies (%ux%u, %.1fm, resolution %u), %zu vegetation profiles, %zu instances per chunk and %zu render proxies (%zu profile loads not ready, %zu without render data), %zu sculpt and %zu paint stamps, revision %llu.",
			chunksToBuild.Num(), data.m_chunks.Num(), data.m_physicsBodies.Num(), data.m_chunksX, data.m_chunksZ, data.m_chunkSize,
			data.m_chunkResolution, data.m_vegetationProfiles.Num(), vegetationPerChunk,
			vegetationRenderProxies, vegetationProfilesNotReady,
			vegetationProfilesWithoutRenderData,
			data.m_sculptStamps.Num() / 5u, data.m_paintStamps.Num() / 5u,
			static_cast<unsigned long long>(data.m_buildRevision));
		TrySaveVegetationAsset(data);
	}
	bSceneChanged |= UpdateGrassResidency(cameraTransforms, cameras);
	if (bSceneChanged)
	{
		PublishSceneVersion();
	}
	return nullptr;
}

bool LandscapeECS::UpdateGrassResidency(
	const TVector<Math::Transform>& cameraTransforms,
	const TVector<CameraData>& cameras)
{
	auto findResident = [](const LandscapeChunk& chunk, size_t profileIndex)
		-> const LandscapeVegetationRenderProxy*
		{
			for (const auto& proxy : chunk.m_vegetationProxies)
			{
				if (proxy.m_mobility == EMobilityType::Dynamic &&
					proxy.m_profileIndex == profileIndex)
				{
					return &proxy;
				}
			}
			return nullptr;
		};
	auto toChunkCoordinate = [](double localPosition, double extent, double chunkSize)
		{
			const double coordinate = std::floor(
				(localPosition + extent * 0.5) / chunkSize);
			return static_cast<int32_t>((std::clamp)(
				coordinate,
				static_cast<double>((std::numeric_limits<int32_t>::lowest)()),
				static_cast<double>((std::numeric_limits<int32_t>::max)())));
		};

	const size_t numCameras = (std::min)(cameraTransforms.Num(), cameras.Num());
	m_cameraPositionsScratch.Resize(numCameras);
	m_cameraFrustumsScratch.Resize(numCameras);
	for (size_t cameraIndex = 0u; cameraIndex < numCameras; ++cameraIndex)
	{
		const auto& camera = cameras[cameraIndex];
		const float aspect = std::isfinite(camera.GetAspect()) && camera.GetAspect() > 0.0f ?
			camera.GetAspect() : 1.0f;
		const float fov = std::isfinite(camera.GetFov()) ?
			(std::clamp)(camera.GetFov(), 1.0f, 179.0f) : 90.0f;
		const float zNear = std::isfinite(camera.GetZNear()) ?
			(std::max)(camera.GetZNear(), 0.001f) : 0.1f;
		const float zFar = std::isfinite(camera.GetZFar()) ?
			(std::max)(camera.GetZFar(), zNear + 0.001f) : 1000.0f;
		m_cameraPositionsScratch[cameraIndex] = glm::vec3(
			cameraTransforms[cameraIndex].m_position);
		m_cameraFrustumsScratch[cameraIndex].ExtractFrustumPlanes(
			cameraTransforms[cameraIndex].Matrix(),
			aspect,
			fov,
			zNear,
			zFar);
	}

	m_grassCandidatesScratch.Clear(false);
	for (size_t componentIndex = 0u; componentIndex < m_components.Num(); ++componentIndex)
	{
		if (!IsComponentRegistered(componentIndex))
		{
			continue;
		}
		auto& component = m_components[componentIndex];
		component.m_activeGrassInstances = 0u;
		GameObjectPtr owner = const_cast<ObjectPtr&>(component.GetOwner()).StaticCast<GameObject>();
		if (!owner || component.m_chunks.IsEmpty() || numCameras == 0u)
		{
			continue;
		}

		const glm::mat4 ownerMatrix = owner->GetTransformComponent().GetCachedWorldMatrix();
		const glm::mat4 inverseOwnerMatrix = glm::inverse(ownerMatrix);
		const double landscapeWidth = static_cast<double>(component.m_chunksX) * component.m_chunkSize;
		const double landscapeDepth = static_cast<double>(component.m_chunksZ) * component.m_chunkSize;
		m_cameraChunkCoordinatesScratch.Clear(false);
		for (size_t cameraIndex = 0u; cameraIndex < numCameras; ++cameraIndex)
		{
			const auto& cameraTransform = cameraTransforms[cameraIndex];
			const glm::vec4 localPosition = inverseOwnerMatrix *
				glm::vec4(glm::vec3(cameraTransform.m_position), 1.0f);
			if (!std::isfinite(localPosition.x) || !std::isfinite(localPosition.z))
			{
				continue;
			}
			m_cameraChunkCoordinatesScratch.Add(glm::ivec2(
				toChunkCoordinate(localPosition.x, landscapeWidth, component.m_chunkSize),
				toChunkCoordinate(localPosition.z, landscapeDepth, component.m_chunkSize)));
		}
		if (m_cameraChunkCoordinatesScratch.IsEmpty())
		{
			continue;
		}

		for (size_t chunkIndex = 0u; chunkIndex < component.m_chunks.Num(); ++chunkIndex)
		{
			const auto& chunk = component.m_chunks[chunkIndex];
			if (!chunk.m_resource)
			{
				continue;
			}
			bool bChunkResident = false;
			for (const auto& proxy : chunk.m_vegetationProxies)
			{
				bChunkResident |= proxy.m_mobility == EMobilityType::Dynamic;
			}
			const Math::AABB& worldBounds = chunk.m_resource->m_proxy.m_worldAabb;
			bool bOverlapsCameraFrustum = false;
			for (const auto& frustum : m_cameraFrustumsScratch)
			{
				if (DoesLandscapeGrassChunkOverlapFrustum(
						worldBounds,
						frustum,
						bChunkResident ? component.m_grassResidencyHysteresis : 0.0f))
				{
					bOverlapsCameraFrustum = true;
					break;
				}
			}
			if (!bOverlapsCameraFrustum)
			{
				continue;
			}

			uint32_t chunkRing = (std::numeric_limits<uint32_t>::max)();
			uint32_t chunkManhattanDistance = (std::numeric_limits<uint32_t>::max)();
			for (const glm::ivec2& cameraChunk : m_cameraChunkCoordinatesScratch)
			{
				const uint64_t distanceX = static_cast<uint64_t>(std::abs(
					static_cast<int64_t>(cameraChunk.x) - chunk.m_chunkX));
				const uint64_t distanceZ = static_cast<uint64_t>(std::abs(
					static_cast<int64_t>(cameraChunk.y) - chunk.m_chunkZ));
				const uint32_t ring = static_cast<uint32_t>((std::min)(
					(std::max)(distanceX, distanceZ),
					static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())));
				const uint32_t manhattanDistance = static_cast<uint32_t>((std::min)(
					distanceX + distanceZ,
					static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())));
				if (ring < chunkRing ||
					(ring == chunkRing && manhattanDistance < chunkManhattanDistance))
				{
					chunkRing = ring;
					chunkManhattanDistance = manhattanDistance;
				}
			}

			float minCameraDistance = (std::numeric_limits<float>::infinity)();
			for (size_t cameraIndex = 0u; cameraIndex < numCameras; ++cameraIndex)
			{
				const glm::vec3& cameraPosition = m_cameraPositionsScratch[cameraIndex];
				const glm::vec3 closest = glm::clamp(
					cameraPosition,
					worldBounds.m_min,
					worldBounds.m_max);
				minCameraDistance = (std::min)(
					minCameraDistance,
					glm::distance(cameraPosition, closest));
			}

			for (size_t profileIndex = 0u;
				profileIndex < component.m_vegetationProfiles.Num();
				++profileIndex)
			{
				const auto& profile = component.m_vegetationProfiles[profileIndex];
				if (profile.m_residency != ELandscapeVegetationResidency::Grass ||
					!profile.m_modelFileId)
				{
					continue;
				}
				const uint32_t instanceCapacity = GetVegetationInstanceCapacity(
					component,
					chunk,
					profileIndex);
				if (instanceCapacity == 0u)
				{
					continue;
				}
				const bool bResident = findResident(chunk, profileIndex) != nullptr;
				const float residencyDistance = profile.m_cullDistance +
					(bResident ? component.m_grassResidencyHysteresis : 0.0f);
				if (minCameraDistance > residencyDistance)
				{
					continue;
				}

				LandscapeGrassCandidate candidate;
				candidate.m_componentIndex = componentIndex;
				candidate.m_chunkIndex = chunkIndex;
				candidate.m_profileIndex = profileIndex;
				candidate.m_capacity = instanceCapacity;
				candidate.m_chunkRing = chunkRing;
				candidate.m_chunkManhattanDistance = chunkManhattanDistance;
				candidate.m_priority = profile.m_priority;
				candidate.m_bChunkResident = bChunkResident;
				m_grassCandidatesScratch.Add(std::move(candidate));
			}
		}
	}

	const uint32_t instanceBudget = (std::min)(
		App::GetActiveGraphicsSettings().m_vegetationInstanceBudget,
		1048576u);
	const size_t numCandidates = m_grassCandidatesScratch.Num();
	SelectLandscapeGrassResidency(
		m_grassCandidatesScratch,
		instanceBudget,
		m_grassSelectionsScratch);
	for (auto& selection : m_grassSelectionsScratch)
	{
		if (!IsComponentRegistered(selection.m_componentIndex))
		{
			continue;
		}
		auto& component = m_components[selection.m_componentIndex];
		if (selection.m_profileIndex >= component.m_vegetationProfiles.Num())
		{
			continue;
		}
		if (selection.m_chunkIndex >= component.m_chunks.Num())
		{
			continue;
		}
		const uint32_t instanceCapacity = GetVegetationInstanceCapacity(
			component,
			component.m_chunks[selection.m_chunkIndex],
			selection.m_profileIndex);
		if (selection.m_instanceCount >= instanceCapacity)
		{
			continue;
		}
		GameObjectPtr owner = const_cast<ObjectPtr&>(component.GetOwner()).StaticCast<GameObject>();
		if (!owner)
		{
			continue;
		}
		selection.m_viewRevision = CalculateGrassViewRevision(
			component,
			instanceCapacity,
			glm::inverse(owner->GetTransformComponent().GetCachedWorldMatrix()),
			m_cameraPositionsScratch);
	}
	m_grassSelectionsScratch.Sort([](
		const LandscapeGrassSelection& lhs,
		const LandscapeGrassSelection& rhs)
		{
			if (lhs.m_componentIndex != rhs.m_componentIndex)
			{
				return lhs.m_componentIndex < rhs.m_componentIndex;
			}
			if (lhs.m_chunkIndex != rhs.m_chunkIndex)
			{
				return lhs.m_chunkIndex < rhs.m_chunkIndex;
			}
			return lhs.m_profileIndex < rhs.m_profileIndex;
		});
	auto findSelection = [this](
		size_t componentIndex,
		size_t chunkIndex,
		size_t profileIndex) -> const LandscapeGrassSelection*
		{
			size_t first = 0u;
			size_t last = m_grassSelectionsScratch.Num();
			while (first < last)
			{
				const size_t middle = first + (last - first) / 2u;
				const auto& selection = m_grassSelectionsScratch[middle];
				const bool bBefore = selection.m_componentIndex < componentIndex ||
					(selection.m_componentIndex == componentIndex &&
						(selection.m_chunkIndex < chunkIndex ||
							(selection.m_chunkIndex == chunkIndex &&
								selection.m_profileIndex < profileIndex)));
				if (bBefore)
				{
					first = middle + 1u;
				}
				else
				{
					last = middle;
				}
			}
			if (first >= m_grassSelectionsScratch.Num())
			{
				return nullptr;
			}
			const auto& selection = m_grassSelectionsScratch[first];
			return selection.m_componentIndex == componentIndex &&
				selection.m_chunkIndex == chunkIndex &&
				selection.m_profileIndex == profileIndex ?
				&selection : nullptr;
		};

	m_grassBuildRequestsScratch.Clear(false);
	for (size_t componentIndex = 0u; componentIndex < m_components.Num(); ++componentIndex)
	{
		if (!IsComponentRegistered(componentIndex))
		{
			continue;
		}
		auto& component = m_components[componentIndex];
		GameObjectPtr owner = const_cast<ObjectPtr&>(component.GetOwner()).StaticCast<GameObject>();
		if (!owner)
		{
			continue;
		}
		const glm::mat4 ownerMatrix = owner->GetTransformComponent().GetCachedWorldMatrix();
		for (size_t chunkIndex = 0u; chunkIndex < component.m_chunks.Num(); ++chunkIndex)
		{
			auto& chunk = component.m_chunks[chunkIndex];
			for (size_t profileIndex = 0u;
				profileIndex < component.m_vegetationProfiles.Num();
				++profileIndex)
			{
				const auto& profile = component.m_vegetationProfiles[profileIndex];
				if (profile.m_residency != ELandscapeVegetationResidency::Grass)
				{
					continue;
				}
				const auto* resident = findResident(chunk, profileIndex);
				const uint32_t residentCount = resident ? resident->m_instanceCount : 0u;
				const auto* selection = findSelection(
					componentIndex,
					chunkIndex,
					profileIndex);
				const uint32_t selectedCount = selection ? selection->m_instanceCount : 0u;
				const uint32_t instanceCapacity = GetVegetationInstanceCapacity(
					component,
					chunk,
					profileIndex);
				const bool bPartialSelection = selection &&
					selectedCount < instanceCapacity;
				if (selectedCount == 0u ||
					(residentCount == selectedCount &&
						(!bPartialSelection ||
							resident->m_viewRevision == selection->m_viewRevision)))
				{
					continue;
				}

				const LandscapeData* componentData = &component;
				const LandscapeChunk* chunkData = &chunk;
				auto task = Tasks::CreateTask<LandscapeVegetationRenderInstances>(
					"LandscapeECS:Build Grass Transforms",
					[componentData,
						chunkData,
						profileIndex,
						selectedCount,
						ownerMatrix,
						this]()
					{
						return BuildGrassInstanceTransforms(
							*componentData,
							*chunkData,
							profileIndex,
							selectedCount,
							ownerMatrix,
							m_cameraPositionsScratch);
					},
					EThreadType::Worker);
				task->Run();
				GrassTransformBuildRequest request;
				request.m_componentIndex = componentIndex;
				request.m_chunkIndex = chunkIndex;
				request.m_profileIndex = profileIndex;
				request.m_instanceCount = selectedCount;
				request.m_viewRevision = selection->m_viewRevision;
				request.m_task = std::move(task);
				m_grassBuildRequestsScratch.Add(std::move(request));
			}
		}
	}

	bool bChanged = false;
	uint32_t activeInstances = 0u;
	const uint64_t frame = GetWorld()->GetCurrentFrame();
	auto findBuildRequest = [this](
		size_t componentIndex,
		size_t chunkIndex,
		size_t profileIndex) -> GrassTransformBuildRequest*
		{
			size_t first = 0u;
			size_t last = m_grassBuildRequestsScratch.Num();
			while (first < last)
			{
				const size_t middle = first + (last - first) / 2u;
				const auto& request = m_grassBuildRequestsScratch[middle];
				const bool bBefore = request.m_componentIndex < componentIndex ||
					(request.m_componentIndex == componentIndex &&
						(request.m_chunkIndex < chunkIndex ||
							(request.m_chunkIndex == chunkIndex &&
								request.m_profileIndex < profileIndex)));
				if (bBefore)
				{
					first = middle + 1u;
				}
				else
				{
					last = middle;
				}
			}
			if (first >= m_grassBuildRequestsScratch.Num())
			{
				return nullptr;
			}
			auto& request = m_grassBuildRequestsScratch[first];
			return request.m_componentIndex == componentIndex &&
				request.m_chunkIndex == chunkIndex &&
				request.m_profileIndex == profileIndex ?
				&request : nullptr;
		};
	for (size_t componentIndex = 0u; componentIndex < m_components.Num(); ++componentIndex)
	{
		if (!IsComponentRegistered(componentIndex))
		{
			continue;
		}
		auto& component = m_components[componentIndex];
		uint32_t componentActiveInstances = 0u;
		GameObjectPtr owner = const_cast<ObjectPtr&>(component.GetOwner()).StaticCast<GameObject>();
		const glm::mat4 ownerMatrix = owner ?
			owner->GetTransformComponent().GetCachedWorldMatrix() : glm::mat4(1.0f);
		for (size_t chunkIndex = 0u; chunkIndex < component.m_chunks.Num(); ++chunkIndex)
		{
			auto& chunk = component.m_chunks[chunkIndex];
			bool bNeedsUpdate = false;
			for (size_t profileIndex = 0u;
				profileIndex < component.m_vegetationProfiles.Num();
				++profileIndex)
			{
				const auto& profile = component.m_vegetationProfiles[profileIndex];
				if (profile.m_residency != ELandscapeVegetationResidency::Grass)
				{
					continue;
				}
				const auto* resident = findResident(chunk, profileIndex);
				const uint32_t residentCount = resident ? resident->m_instanceCount : 0u;
				const auto* selection = findSelection(
					componentIndex,
					chunkIndex,
					profileIndex);
				const uint32_t selectedCount = selection ? selection->m_instanceCount : 0u;
				const uint32_t instanceCapacity = GetVegetationInstanceCapacity(
					component,
					chunk,
					profileIndex);
				const bool bPartialSelection = selection &&
					selectedCount < instanceCapacity;
				bNeedsUpdate |= residentCount != selectedCount ||
					(resident && bPartialSelection &&
						resident->m_viewRevision != selection->m_viewRevision);
				componentActiveInstances += residentCount;
			}
			if (!bNeedsUpdate)
			{
				continue;
			}

			TVector<LandscapeVegetationRenderProxy> nextProxies;
			nextProxies.Reserve(chunk.m_vegetationProxies.Num());
			for (const auto& proxy : chunk.m_vegetationProxies)
			{
				if (proxy.m_mobility != EMobilityType::Dynamic)
				{
					nextProxies.Add(proxy);
				}
			}
			bool bChunkChanged = false;
			for (size_t profileIndex = 0u;
				profileIndex < component.m_vegetationProfiles.Num();
				++profileIndex)
			{
				const auto& profile = component.m_vegetationProfiles[profileIndex];
				if (profile.m_residency != ELandscapeVegetationResidency::Grass)
				{
					continue;
				}

				const auto* resident = findResident(chunk, profileIndex);
				const auto* selection = findSelection(
					componentIndex,
					chunkIndex,
					profileIndex);
				const uint32_t selectedCount = selection ? selection->m_instanceCount : 0u;
				if (selectedCount == 0u)
				{
					bChunkChanged |= resident != nullptr;
					if (resident)
					{
						componentActiveInstances -= resident->m_instanceCount;
					}
					continue;
				}
				const bool bPartialSelection =
					selectedCount < GetVegetationInstanceCapacity(
						component,
						chunk,
						profileIndex);
				if (resident && resident->m_instanceCount == selectedCount &&
					(!bPartialSelection ||
						resident->m_viewRevision == selection->m_viewRevision))
				{
					nextProxies.Add(*resident);
					continue;
				}
				if (!owner)
				{
					continue;
				}

				auto* buildRequest = findBuildRequest(
					componentIndex,
					chunkIndex,
					profileIndex);
				if (!buildRequest ||
					buildRequest->m_instanceCount != selectedCount ||
					buildRequest->m_viewRevision != selection->m_viewRevision)
				{
					if (resident)
					{
						nextProxies.Add(*resident);
					}
					continue;
				}
				buildRequest->m_task->Wait();
				auto instances = std::move(buildRequest->m_task->m_result);
				LandscapeVegetationRenderProxy streamedProxy;
				const uint64_t revision = (uint64_t(1u) << 63u) |
					++component.m_streamingRevision;
				const auto buildResult = BuildLandscapeVegetationProxy(
					componentIndex,
					chunkIndex,
					profileIndex,
					profile,
					ownerMatrix,
					frame,
					std::move(instances),
					EMobilityType::Dynamic,
					revision,
					streamedProxy);
				if (buildResult != EVegetationProxyBuildResult::Success)
				{
					if (resident && resident->m_instanceCount <= selectedCount)
					{
						nextProxies.Add(*resident);
					}
					else if (resident)
					{
						componentActiveInstances -= resident->m_instanceCount;
						bChunkChanged = true;
					}
					continue;
				}
				streamedProxy.m_viewRevision = selection->m_viewRevision;
				if (resident)
				{
					componentActiveInstances -= resident->m_instanceCount;
				}
				nextProxies.Add(std::move(streamedProxy));
				componentActiveInstances += selectedCount;
				bChunkChanged = true;
			}

			if (bChunkChanged)
			{
				chunk.m_vegetationProxies = std::move(nextProxies);
				bChanged = true;
			}
		}
		component.m_activeGrassInstances = componentActiveInstances;
		activeInstances += componentActiveInstances;
	}
	for (auto& buildRequest : m_grassBuildRequestsScratch)
	{
		buildRequest.m_task->Wait();
	}
	if (bChanged)
	{
		++m_shadowCastersRevision;
		SAILOR_LOG(
			"LandscapeECS: grass residency changed to %u of %u graphics-quality instances across %zu visible candidates.",
			activeInstances,
			instanceBudget,
			numCandidates);
	}
	return bChanged;
}

void LandscapeECS::PublishSceneVersion()
{
	auto version = RHI::RHISpatialSceneVersionPtr::Make();
	version->m_revision = ++m_sceneVersionRevision;
	version->m_shadowCastersRevision = m_shadowCastersRevision;
	version->m_scene = m_rhiScene;
	TSet<size_t> activeProducerKeys;
	size_t staticSpatialHash = 1469598103934665603ull;
	size_t dynamicSpatialHash = 1099511628211ull;
	bool bHasStaticSpatialEntries = false;
	bool bHasDynamicSpatialEntries = false;
	auto hashSpatialEntry = [&staticSpatialHash,
		&dynamicSpatialHash,
		&bHasStaticSpatialEntries,
		&bHasDynamicSpatialEntries](
		const RHI::RenderInstanceHandle& handle,
		const glm::ivec3& center,
		const glm::ivec3& extents,
		EMobilityType mobility)
		{
			if (mobility == EMobilityType::Dynamic)
			{
				bHasDynamicSpatialEntries = true;
			}
			else
			{
				bHasStaticSpatialEntries = true;
			}
			size_t& spatialHash = mobility == EMobilityType::Dynamic ?
				dynamicSpatialHash : staticSpatialHash;
			HashCombine(
				spatialHash,
				handle.m_slot,
				handle.m_generation,
				center.x,
				center.y,
				center.z,
				extents.x,
				extents.y,
				extents.z);
		};

	auto publishProxy = [this, &activeProducerKeys](
		const RHI::RHISceneProxyResourcePtr& resource,
		uint64_t buildRevision,
		EMobilityType mobility) -> RHI::RenderInstanceHandle
		{
			if (!m_rhiScene || !resource)
			{
				return {};
			}

			const auto& proxy = resource->m_proxy;
			const size_t producerKey = proxy.m_staticMeshEcs;
			activeProducerKeys.Insert(producerKey);
			RHI::RenderInstanceHandle* handle = nullptr;
			uint64_t* publishedRevision = nullptr;
			if (m_renderInstanceHandles.Find(producerKey, handle) && handle &&
				m_publishedBuildRevisions.Find(producerKey, publishedRevision) &&
				publishedRevision && *publishedRevision == buildRevision)
			{
				return *handle;
			}

			RHI::RHISceneInstanceRecord record;
			record.m_producerKey = producerKey;
			record.m_mobility = mobility;
			record.m_worldMatrix = proxy.m_worldMatrix;
			record.m_worldBounds = proxy.m_worldAabb;
			record.m_topology = resource;
			record.m_topologyRevision = buildRevision;
			record.m_materialRevision = Material::GetGlobalContentRevision();
			record.m_shadowRevision = resource->m_shadowRevision;
			record.m_skeletonOffset = proxy.m_skeletonOffset;
			record.m_renderFlags = proxy.m_bCastShadows ? 1u : 0u;

			if (m_renderInstanceHandles.Find(producerKey, handle) && handle)
			{
				m_rhiScene->UpdateInstance(
					*handle,
					record,
					RHI::ToMask(RHI::ESceneChangeBit::ReplaceChunkRange) |
					RHI::ToMask(RHI::ESceneChangeBit::MeshOrLodTopology) |
					RHI::ToMask(RHI::ESceneChangeBit::Material) |
					RHI::ToMask(RHI::ESceneChangeBit::ShadowState));
			}
			else
			{
				m_renderInstanceHandles[producerKey] = m_rhiScene->AddInstance(record);
				handle = &m_renderInstanceHandles[producerKey];
			}
			m_publishedBuildRevisions[producerKey] = buildRevision;
			return handle ? *handle : RHI::RenderInstanceHandle{};
		};

	for (size_t componentIndex = 0; componentIndex < m_components.Num(); ++componentIndex)
	{
		if (!IsComponentRegistered(componentIndex))
		{
			continue;
		}

		const auto& data = m_components[componentIndex];
		for (const auto& chunk : data.m_chunks)
		{
			const auto chunkHandle = publishProxy(
				chunk.m_resource,
				chunk.m_buildRevision,
				EMobilityType::Static);
			if (chunkHandle.IsValid())
			{
				hashSpatialEntry(
					chunkHandle,
					chunk.m_octreeCenter,
					chunk.m_octreeExtents,
					EMobilityType::Static);
			}
			for (const auto& vegetation : chunk.m_vegetationProxies)
			{
				const auto vegetationHandle = publishProxy(
					vegetation.m_resource,
					vegetation.m_revision,
					vegetation.m_mobility);
				if (vegetationHandle.IsValid())
				{
					hashSpatialEntry(
						vegetationHandle,
						vegetation.m_octreeCenter,
						vegetation.m_octreeExtents,
						vegetation.m_mobility);
				}
			}
		}

		for (const auto& profile : data.m_vegetationProfiles)
		{
			version->m_bHasCustomDepthShadowCasters |= profile.m_material &&
				profile.m_shadowMode != ELandscapeVegetationShadowMode::None &&
				profile.m_material->GetRenderState().IsRequiredCustomDepthShader();
		}
	}
	if (m_rhiScene)
	{
		for (size_t producerKey : m_renderInstanceHandles.GetKeys())
		{
			if (activeProducerKeys.Contains(producerKey))
			{
				continue;
			}
			RHI::RenderInstanceHandle* handle = nullptr;
			if (m_renderInstanceHandles.Find(producerKey, handle) && handle)
			{
				m_rhiScene->RemoveInstance(*handle);
			}
			m_renderInstanceHandles.Remove(producerKey);
			m_publishedBuildRevisions.Remove(producerKey);
		}

		const bool bStaticSpatialChanged = !m_publishedSceneVersion ||
			m_staticSpatialHash != staticSpatialHash;
		const bool bDynamicSpatialChanged = !m_publishedSceneVersion ||
			m_dynamicSpatialHash != dynamicSpatialHash;
		if (bStaticSpatialChanged || bDynamicSpatialChanged)
		{
			++m_spatialRevision;
		}

		auto rebuildSpatialTree = [this](
			EMobilityType mobility,
			bool bHasEntries,
			TSharedPtr<TOctree<RHI::RenderInstanceHandle>>& tree)
			{
				if (!bHasEntries)
				{
					tree.Clear();
					return;
				}
				tree = TSharedPtr<TOctree<RHI::RenderInstanceHandle>>::Make(
					glm::ivec3(0, 0, 0), 16536 * 16, 4);
				auto appendSpatialEntry = [this, &tree](
					const RHI::RHISceneProxyResourcePtr& resource,
					const glm::ivec3& center,
					const glm::ivec3& extents)
					{
						if (!resource)
						{
							return;
						}
						RHI::RenderInstanceHandle* handle = nullptr;
						if (m_renderInstanceHandles.Find(
							resource->m_proxy.m_staticMeshEcs,
							handle) && handle)
						{
							tree->Update(center, extents, *handle);
						}
					};
				for (size_t componentIndex = 0u;
					componentIndex < m_components.Num();
					++componentIndex)
				{
					if (!IsComponentRegistered(componentIndex))
					{
						continue;
					}
					for (const auto& chunk : m_components[componentIndex].m_chunks)
					{
						if (mobility == EMobilityType::Static)
						{
							appendSpatialEntry(
								chunk.m_resource,
								chunk.m_octreeCenter,
								chunk.m_octreeExtents);
						}
						for (const auto& vegetation : chunk.m_vegetationProxies)
						{
							if (vegetation.m_mobility == mobility)
							{
								appendSpatialEntry(
									vegetation.m_resource,
									vegetation.m_octreeCenter,
									vegetation.m_octreeExtents);
							}
						}
					}
				}
			};

		if (bStaticSpatialChanged)
		{
			rebuildSpatialTree(
				EMobilityType::Static,
				bHasStaticSpatialEntries,
				version->m_staticOctree);
		}
		else
		{
			version->m_staticOctree = m_publishedSceneVersion->m_staticOctree;
		}
		if (bDynamicSpatialChanged)
		{
			rebuildSpatialTree(
				EMobilityType::Dynamic,
				bHasDynamicSpatialEntries,
				version->m_dynamicOctree);
		}
		else
		{
			version->m_dynamicOctree = m_publishedSceneVersion->m_dynamicOctree;
		}
		m_staticSpatialHash = staticSpatialHash;
		m_dynamicSpatialHash = dynamicSpatialHash;
		version->m_sceneVersion = m_rhiScene->PublishVersion(
			Material::GetGlobalContentRevision(),
			m_shadowCastersRevision,
			m_spatialRevision);
	}

	m_publishedSceneVersion = std::move(version);
}

void LandscapeECS::AppendSceneView(RHI::RHISceneViewPtr& sceneView) const
{
	if (!sceneView)
	{
		return;
	}
	sceneView->AddSceneVersion(m_publishedSceneVersion);
}

void LandscapeECS::EndPlay()
{
	for (auto& component : m_components)
	{
		DestroyPhysicsBodies(component);
	}
	ECS::TSystem<LandscapeECS, LandscapeData>::EndPlay();
	m_shadowCastersRevision = 0u;
	m_sceneVersionRevision = 0u;
	m_spatialRevision = 0u;
	m_staticSpatialHash = 0u;
	m_dynamicSpatialHash = 0u;
	m_publishedSceneVersion.Clear();
	m_rhiScene.Clear();
	m_renderInstanceHandles.Clear();
	m_publishedBuildRevisions.Clear();
}
