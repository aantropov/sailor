#include "ECS/LandscapeECS.h"

#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Components/LandscapeComponent.h"
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
		struct VegetationInstance
		{
			size_t m_profileIndex = 0u;
			glm::vec3 m_position{};
			float m_angle = 0.0f;
			float m_scale = 1.0f;
		};

		uint32_t m_chunkX = 0u;
		uint32_t m_chunkZ = 0u;
		TVector<RHI::VertexP3N3T3B3UV2C4> m_vertices{};
		TVector<uint32_t> m_indices{};
		TVector<VegetationInstance> m_vegetation{};
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

		result.m_indices.Reserve(static_cast<size_t>(resolution) * resolution * 6u);
		for (uint32_t z = 0u; z < resolution; ++z)
		{
			for (uint32_t x = 0u; x < resolution; ++x)
			{
				const uint32_t i0 = z * row + x;
				const uint32_t i1 = i0 + 1u;
				const uint32_t i2 = i0 + row;
				const uint32_t i3 = i2 + 1u;
				result.m_indices.AddRange({ i0, i2, i1, i1, i2, i3 });
			}
		}

		for (size_t profileIndex = 0u; profileIndex < data.m_vegetationProfiles.Num(); ++profileIndex)
		{
			const auto& profile = data.m_vegetationProfiles[profileIndex];
			if (!profile.m_modelFileId)
			{
				continue;
			}
			result.m_vegetation.Reserve(result.m_vegetation.Num() + profile.m_instancesPerChunk);
			for (uint32_t instance = 0u; instance < profile.m_instancesPerChunk; ++instance)
			{
				const uint32_t randomSeed = data.m_seed ^
					static_cast<uint32_t>(chunkX * 92821u + chunkZ * 68917u +
						profileIndex * 4099u + instance * 131u);
				LandscapeChunkCpuData::VegetationInstance placement;
				placement.m_profileIndex = profileIndex;
				placement.m_position.x = originX + Random01(randomSeed) * data.m_chunkSize;
				placement.m_position.z = originZ + Random01(randomSeed + 1u) * data.m_chunkSize;
				placement.m_position.y = SampleHeight(placement.m_position.x, placement.m_position.z,
					landscapeWidth, landscapeDepth, data.m_noiseScale, data.m_heightScale,
					data.m_seed, data.m_sculptStamps, heightmap) +
					profile.m_groundOffset;
				placement.m_angle = Random01(randomSeed + 2u) * glm::two_pi<float>();
				placement.m_scale = glm::mix(profile.m_minScale, profile.m_maxScale,
					Random01(randomSeed + 3u));
				result.m_vegetation.Add(std::move(placement));
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
			shadowMesh.m_customDepthMaterial = material;
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
}

void LandscapeData::SetSettings(uint32_t chunksX, uint32_t chunksZ,
	float chunkSize, uint32_t chunkResolution, float heightScale,
	float noiseScale, uint32_t seed, float textureTiling)
{
	m_chunksX = (std::clamp)(chunksX, 1u, 64u);
	m_chunksZ = (std::clamp)(chunksZ, 1u, 64u);
	m_chunkSize = (std::max)(chunkSize, 1.0f);
	m_chunkResolution = (std::clamp)(chunkResolution, 2u, 128u);
	m_heightScale = (std::max)(heightScale, 0.0f);
	m_noiseScale = (std::max)(noiseScale, 0.0001f);
	m_seed = seed;
	m_textureTiling = (std::max)(textureTiling, 0.001f);
	MarkDirty();
}

void LandscapeData::SetMaterial(const MaterialPtr& material)
{
	m_material = material;
	m_runtimeMaterial.Clear();
	MarkDirty();
}

void LandscapeData::SetLayerTextures(const TVector<FileId>& textures)
{
	m_layerTextures = textures;
	if (m_layerTextures.Num() > 4u) m_layerTextures.Resize(4u);
	m_runtimeMaterial.Clear();
	MarkDirty();
}

void LandscapeData::SetImportMaps(const FileId& heightmapTexture,
	const TVector<FileId>& materialMasks)
{
	m_heightmapTexture = heightmapTexture;
	m_materialMasks = materialMasks;
	if (m_materialMasks.Num() > 4u) m_materialMasks.Resize(4u);
	MarkDirty();
}

void LandscapeData::SetAuthoredStamps(const TVector<float>& sculptStamps,
	const TVector<float>& paintStamps)
{
	m_sculptStamps = sculptStamps;
	m_paintStamps = paintStamps;
	MarkDirty();
}

void LandscapeData::SetVegetationProfiles(
	const TVector<FileId>& models,
	const TVector<FileId>& materials,
	const TVector<float>& meshIndex,
	const TVector<float>& instancesPerChunk,
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
	m_vegetationProfiles.Clear();
	const size_t numProfiles = models.Num();
	m_vegetationProfiles.Reserve(numProfiles);
	for (size_t index = 0u; index < numProfiles; ++index)
	{
		LandscapeVegetationProfile profile;
		profile.m_modelFileId = models[index];
		profile.m_materialFileId = index < materials.Num() ? materials[index] : FileId{};
		profile.m_meshIndex = static_cast<int32_t>((std::clamp)(
			GetProfileValue(meshIndex, index, -1.0f), -1.0f, 65535.0f));
		profile.m_instancesPerChunk = static_cast<uint32_t>((std::clamp)(
			GetProfileValue(instancesPerChunk, index, 0.0f), 0.0f, 2048.0f));
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
		m_vegetationProfiles.Add(std::move(profile));
	}
	MarkDirty();
}

void LandscapeECS::BeginPlay()
{
}

void LandscapeECS::OnComponentUnregistered(size_t, LandscapeData& component)
{
	DestroyPhysicsBodies(component);
	component.m_chunks.Clear();
	component.m_runtimeMaterial.Clear();
	++m_shadowCastersRevision;
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
}

Tasks::ITaskPtr LandscapeECS::Tick(float)
{
	SAILOR_PROFILE_FUNCTION();
	for (size_t componentIndex = 0; componentIndex < m_components.Num(); ++componentIndex)
	{
		if (!IsComponentRegistered(componentIndex))
		{
			continue;
		}

		auto& data = m_components[componentIndex];
		GameObjectPtr owner = const_cast<ObjectPtr&>(data.GetOwner()).StaticCast<GameObject>();
		const bool transformChanged = owner && owner->GetTransformComponent().GetFrameLastChange() > data.GetFrameLastChange();
		if (!data.IsDirty() && !transformChanged)
		{
			continue;
		}
		if (!owner || !data.m_material || !data.m_material->IsReady())
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
		tasks.Reserve(static_cast<size_t>(data.m_chunksX) * data.m_chunksZ);
		for (uint32_t z = 0; z < data.m_chunksZ; ++z)
		{
			for (uint32_t x = 0; x < data.m_chunksX; ++x)
			{
				auto task = Tasks::CreateTask<LandscapeChunkCpuData>(
					"LandscapeECS:Build Chunk", [&data, &heightmap, &materialMasks, x, z]()
					{
						return BuildChunk(data, heightmap, materialMasks, x, z);
					}, EThreadType::Worker);
				task->Run();
				tasks.Add(task);
			}
		}

		data.m_chunks.Clear();
		DestroyPhysicsBodies(data);
		data.m_chunks.Reserve(tasks.Num());
		size_t vegetationRenderProxies = 0u;
		size_t vegetationProfilesNotReady = 0u;
		size_t vegetationProfilesWithoutRenderData = 0u;
		bool bVegetationResourcesPending = false;
		const glm::mat4 ownerMatrix = owner->GetTransformComponent().GetCachedWorldMatrix();
		const Math::Transform ownerTransform = Math::Transform::FromMatrix(ownerMatrix);
		auto* physics = GetWorld()->GetECS<PhysicsECS>();
		for (size_t chunkIndex = 0; chunkIndex < tasks.Num(); ++chunkIndex)
		{
			tasks[chunkIndex]->Wait();
			auto& cpu = tasks[chunkIndex]->m_result;
			TVector<glm::vec3> collisionVertices;
			collisionVertices.Resize(cpu.m_vertices.Num());
			for (size_t vertexIndex = 0u; vertexIndex < cpu.m_vertices.Num(); ++vertexIndex)
			{
				collisionVertices[vertexIndex] = cpu.m_vertices[vertexIndex].m_position;
			}
			uint32_t physicsBodyId = RigidBodyData::InvalidBodyId;
			if (physics && physics->CreateStaticTriangleMesh(owner->GetInstanceId(),
				collisionVertices, cpu.m_indices, glm::vec3(ownerTransform.m_position),
				ownerTransform.m_rotation, glm::vec3(ownerTransform.m_scale), physicsBodyId))
			{
				data.m_physicsBodies.Add(physicsBodyId);
			}
			TVector<Physics::CollisionShapeDesc> vegetationCollisionShapes;
			for (const auto& placement : cpu.m_vegetation)
			{
				const auto& profile = data.m_vegetationProfiles[placement.m_profileIndex];
				if (profile.m_colliderRadius <= 0.0f)
				{
					continue;
				}
				Physics::CollisionShapeDesc shape;
				shape.m_type = Physics::ECollisionShapeType::Capsule;
				shape.m_center = placement.m_position + glm::vec3(
					0.0f, profile.m_colliderOffsetY * placement.m_scale, 0.0f);
				shape.m_rotation = glm::angleAxis(placement.m_angle, glm::vec3(0.0f, 1.0f, 0.0f));
				shape.m_radius = profile.m_colliderRadius * placement.m_scale;
				shape.m_height = profile.m_colliderHeight * placement.m_scale;
				vegetationCollisionShapes.Add(std::move(shape));
			}
			if (physics && !vegetationCollisionShapes.IsEmpty() &&
				physics->CreateStaticCompound(owner->GetInstanceId(), vegetationCollisionShapes,
					glm::vec3(ownerTransform.m_position), ownerTransform.m_rotation,
					glm::vec3(ownerTransform.m_scale), physicsBodyId))
			{
				data.m_physicsBodies.Add(physicsBodyId);
			}
			auto mesh = RHI::Renderer::GetDriver()->CreateMesh();
			mesh->m_vertexDescription = RHI::Renderer::GetDriver()->GetOrAddVertexDescription<RHI::VertexP3N3T3B3UV2C4>();
			mesh->m_bounds = cpu.m_localBounds;
			mesh->m_materialIndex = 0u;
			RHI::Renderer::GetDriver()->UpdateMesh(mesh,
				cpu.m_vertices.GetData(), cpu.m_vertices.Num() * sizeof(RHI::VertexP3N3T3B3UV2C4),
				cpu.m_indices.GetData(), cpu.m_indices.Num() * sizeof(uint32_t));

			LandscapeChunk chunk;
			auto& proxy = chunk.m_proxy;
			proxy.m_staticMeshEcs = LandscapeProxyId(componentIndex, chunkIndex);
			proxy.m_worldMatrix = ownerMatrix;
			proxy.m_worldAabb = cpu.m_localBounds;
			proxy.m_worldAabb.Apply(ownerMatrix);
			proxy.m_frame = GetWorld()->GetCurrentFrame();
			proxy.m_bCastShadows = true;
			proxy.m_meshes.Add(mesh);
			proxy.m_meshModelMatrices.Add(ownerMatrix);
			proxy.m_overrideMaterials.Add(data.m_runtimeMaterial->GetOrAddRHI(mesh->m_vertexDescription));
			proxy.m_renderQueueTags.Add(data.m_runtimeMaterial->GetRenderState().GetTag());
			auto shadowCaster = RHI::RHIShadowCasterProxyPtr::Make();
			shadowCaster->m_staticMeshEcs = proxy.m_staticMeshEcs;
			shadowCaster->m_skeletonOffset = (std::numeric_limits<uint32_t>::max)();
			shadowCaster->m_frame = proxy.m_frame;
			AppendShadowMesh(*shadowCaster, mesh, ownerMatrix, data.m_runtimeMaterial,
				(std::numeric_limits<float>::max)());

			shadowCaster->m_worldAabb = proxy.m_worldAabb;
			proxy.m_shadowCaster = shadowCaster->m_meshes.IsEmpty() ?
				RHI::RHIShadowCasterProxyPtr{} : shadowCaster;
#if defined(__APPLE__)
			proxy.m_materialTextureSamplers.Resize(1u);
			auto* textureImporter = App::GetSubmodule<TextureImporter>();
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
				const bool bUseMaterialOverride = static_cast<bool>(profile.m_materialFileId);
				if (!profile.m_model || !profile.m_model->IsReady() ||
					(bUseMaterialOverride && (!profile.m_material || !profile.m_material->IsReady())))
				{
					++vegetationProfilesNotReady;
					bVegetationResourcesPending = bVegetationResourcesPending ||
						(profile.m_model && (!bUseMaterialOverride || profile.m_material));
					continue;
				}

				TVector<RHI::RHIMeshPtr> vegetationMeshes;
				TVector<glm::mat4> vegetationModelMatrices;
				Math::AABB vegetationBounds;
				if (!profile.m_model->CollectRenderData(profile.m_meshIndex,
					vegetationMeshes, vegetationModelMatrices, vegetationBounds))
				{
					++vegetationProfilesWithoutRenderData;
					continue;
				}
				TVector<MaterialPtr> vegetationMaterials;
				vegetationMaterials.Reserve(vegetationMeshes.Num());
				bool bVegetationMaterialsReady = true;
				for (size_t meshIndex = 0u; meshIndex < vegetationMeshes.Num(); ++meshIndex)
				{
					MaterialPtr material = profile.m_material;
					if (!bUseMaterialOverride)
					{
						const size_t materialIndex = vegetationMeshes[meshIndex]->ResolveMaterialIndex(
							meshIndex, profile.m_modelMaterials.Num());
						material = materialIndex < profile.m_modelMaterials.Num() ?
							profile.m_modelMaterials[materialIndex] : MaterialPtr{};
					}
					if (!material || !material->IsReady())
					{
						bVegetationMaterialsReady = false;
						break;
					}
					vegetationMaterials.Add(std::move(material));
				}
				if (!bVegetationMaterialsReady)
				{
					++vegetationProfilesNotReady;
					bVegetationResourcesPending = bVegetationResourcesPending ||
						(!bUseMaterialOverride && !profile.m_modelMaterials.IsEmpty());
					continue;
				}
				LandscapeVegetationRenderProxy vegetationRenderProxy;
				auto& vegetationProxy = vegetationRenderProxy.m_proxy;
				vegetationProxy.m_staticMeshEcs = LandscapeVegetationProxyId(
					componentIndex, chunkIndex, profileIndex);
				vegetationProxy.m_worldMatrix = ownerMatrix;
				vegetationProxy.m_frame = proxy.m_frame;
				vegetationProxy.m_bCastShadows =
					profile.m_shadowMode != ELandscapeVegetationShadowMode::None;
				vegetationProxy.m_lodPolicy.m_bEnabled = true;
				vegetationProxy.m_lodPolicy.m_minLod = profile.m_minLod;
				vegetationProxy.m_lodPolicy.m_maxLod = profile.m_maxLod;
				vegetationProxy.m_lodPolicy.m_screenCoverageThresholds =
					profile.m_screenCoverageThresholds;
				vegetationProxy.m_lodPolicy.m_maxCameraDistance = profile.m_cullDistance;
				auto vegetationShadowCaster = RHI::RHIShadowCasterProxyPtr::Make();
				vegetationShadowCaster->m_staticMeshEcs = vegetationProxy.m_staticMeshEcs;
				vegetationShadowCaster->m_skeletonOffset =
					(std::numeric_limits<uint32_t>::max)();
				vegetationShadowCaster->m_frame = vegetationProxy.m_frame;
				vegetationShadowCaster->m_lodPolicy.m_bEnabled = true;
				vegetationShadowCaster->m_lodPolicy.m_minLod = profile.m_minLod;
				vegetationShadowCaster->m_lodPolicy.m_maxLod = profile.m_maxLod;
				vegetationShadowCaster->m_lodPolicy.m_screenCoverageThresholds =
					profile.m_screenCoverageThresholds;
				Math::AABB batchedVegetationBounds;
				const float shadowDistance = (std::min)(profile.m_cullDistance,
					profile.m_shadowMode == ELandscapeVegetationShadowMode::NearOnly ?
					profile.m_shadowDistance : (std::numeric_limits<float>::max)());
				for (const auto& placement : cpu.m_vegetation)
				{
					if (placement.m_profileIndex != profileIndex)
					{
						continue;
					}

					const glm::mat4 instanceMatrix = ownerMatrix *
						glm::translate(glm::mat4(1.0f), placement.m_position) *
						glm::rotate(glm::mat4(1.0f), placement.m_angle, glm::vec3(0.0f, 1.0f, 0.0f)) *
						glm::scale(glm::mat4(1.0f), glm::vec3(placement.m_scale));
					Math::AABB instanceBounds = vegetationBounds;
					instanceBounds.Apply(instanceMatrix);
					batchedVegetationBounds.Extend(instanceBounds);
					for (size_t meshIndex = 0; meshIndex < vegetationMeshes.Num(); ++meshIndex)
					{
						MaterialPtr& material = vegetationMaterials[meshIndex];
						const glm::mat4 meshMatrix = instanceMatrix * vegetationModelMatrices[meshIndex];
						vegetationProxy.m_meshes.Add(vegetationMeshes[meshIndex]);
						vegetationProxy.m_meshModelMatrices.Add(meshMatrix);
						vegetationProxy.m_overrideMaterials.Add(material->GetOrAddRHI(
							vegetationMeshes[meshIndex]->m_vertexDescription));
						vegetationProxy.m_renderQueueTags.Add(material->GetRenderState().GetTag());
						if (profile.m_shadowMode != ELandscapeVegetationShadowMode::None)
						{
							AppendShadowMesh(*vegetationShadowCaster, vegetationMeshes[meshIndex], meshMatrix,
								material, shadowDistance);
						}
					}
				}
				if (vegetationProxy.m_meshes.IsEmpty() || !batchedVegetationBounds.IsValid())
				{
					continue;
				}
				vegetationProxy.m_worldAabb = batchedVegetationBounds;
				vegetationShadowCaster->m_worldAabb = vegetationProxy.m_worldAabb;
				vegetationProxy.m_shadowCaster = vegetationShadowCaster->m_meshes.IsEmpty() ?
					RHI::RHIShadowCasterProxyPtr{} : vegetationShadowCaster;
#if defined(__APPLE__)
				vegetationProxy.m_materialTextureSamplers.Resize(vegetationProxy.m_meshes.Num());
				for (size_t proxyMeshIndex = 0u;
					proxyMeshIndex < vegetationProxy.m_materialTextureSamplers.Num(); ++proxyMeshIndex)
				{
					auto& requested = vegetationProxy.m_materialTextureSamplers[proxyMeshIndex];
					requested.Insert(0u);
					if (textureImporter)
					{
						const MaterialPtr& material = vegetationMaterials[
							proxyMeshIndex % vegetationMaterials.Num()];
						for (const auto& sampler : material->GetSamplers())
						{
							requested.Insert(sampler.m_second ? static_cast<uint32_t>(
								textureImporter->GetTextureIndex(sampler.m_second->GetFileId())) : 0u);
						}
					}
				}
#endif
				GetOctreeBounds(vegetationProxy.m_worldAabb,
					vegetationRenderProxy.m_octreeCenter, vegetationRenderProxy.m_octreeExtents);
				chunk.m_vegetationProxies.Add(std::move(vegetationRenderProxy));
				++vegetationRenderProxies;
			}
			data.m_chunks.Add(std::move(chunk));
		}
		// Model and material import tasks complete before their GPU upload fences do.
		// Keep the component dirty while valid vegetation resources are pending so
		// the next frame can populate the vegetation proxies after those fences signal.
		data.m_bIsDirty = bVegetationResourcesPending;
		data.SetLastChange(owner->GetTransformComponent().GetFrameLastChange());
		++data.m_buildRevision;
		++m_shadowCastersRevision;
		size_t vegetationPerChunk = 0u;
		for (const auto& profile : data.m_vegetationProfiles)
		{
			vegetationPerChunk += profile.m_instancesPerChunk;
		}
		SAILOR_LOG("LandscapeECS: built %zu chunks with %zu collision bodies (%ux%u, %.1fm, resolution %u), %zu vegetation profiles, %zu instances per chunk and %zu render proxies (%zu profile loads not ready, %zu without render data), %zu sculpt and %zu paint stamps, revision %llu.",
			data.m_chunks.Num(), data.m_physicsBodies.Num(), data.m_chunksX, data.m_chunksZ, data.m_chunkSize,
			data.m_chunkResolution, data.m_vegetationProfiles.Num(), vegetationPerChunk,
			vegetationRenderProxies, vegetationProfilesNotReady,
			vegetationProfilesWithoutRenderData,
			data.m_sculptStamps.Num() / 5u, data.m_paintStamps.Num() / 5u,
			static_cast<unsigned long long>(data.m_buildRevision));
	}
	return nullptr;
}

void LandscapeECS::AppendSceneView(RHI::RHISceneViewPtr& sceneView) const
{
	if (!sceneView)
	{
		return;
	}
	for (size_t componentIndex = 0; componentIndex < m_components.Num(); ++componentIndex)
	{
		if (!IsComponentRegistered(componentIndex))
		{
			continue;
		}
		for (const auto& chunk : m_components[componentIndex].m_chunks)
		{
			sceneView->m_staticOctree.Update(chunk.m_octreeCenter, chunk.m_octreeExtents, chunk.m_proxy);
			for (const auto& vegetation : chunk.m_vegetationProxies)
			{
				sceneView->m_staticOctree.Update(
					vegetation.m_octreeCenter,
					vegetation.m_octreeExtents,
					vegetation.m_proxy);
			}
		}
		for (const auto& profile : m_components[componentIndex].m_vegetationProfiles)
		{
			sceneView->m_bHasCustomDepthShadowCasters |= profile.m_material &&
				profile.m_shadowMode != ELandscapeVegetationShadowMode::None &&
				profile.m_material->GetRenderState().IsRequiredCustomDepthShader();
		}
	}
	sceneView->m_shadowCastersRevision =
		sceneView->m_shadowCastersRevision * 1099511628211ull ^ m_shadowCastersRevision;
}

void LandscapeECS::EndPlay()
{
	for (auto& component : m_components)
	{
		DestroyPhysicsBodies(component);
	}
	ECS::TSystem<LandscapeECS, LandscapeData>::EndPlay();
	m_shadowCastersRevision = 0u;
}
