#include "Landscape/LandscapeData.h"
#include "Landscape/LandscapeInternal.h"

#include <algorithm>
#include <cmath>

using namespace Sailor;
using namespace Sailor::LandscapeInternal;

namespace
{
	static void MarkChunksIntersectingStamp(LandscapeData& data,
		glm::vec2 center,
		float brushRadius,
		float margin)
	{
		const float radius = (std::max)(brushRadius, 0.001f) + margin;
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

	template<typename Stamp>
	void MarkChunksAffectedByStampChanges(LandscapeData& data,
		const TVector<Stamp>& previous, const TVector<Stamp>& current, float margin)
	{
		for (size_t index = 0; index < (std::max)(previous.Num(), current.Num()); ++index)
		{
			if (index < previous.Num() && index < current.Num() && previous[index] == current[index]) continue;
			if (index < previous.Num())
			{
				const auto& stamp = previous[index];
				MarkChunksIntersectingStamp(data, { stamp.m_x, stamp.m_z }, stamp.m_radius, margin);
			}
			if (index < current.Num())
			{
				const auto& stamp = current[index];
				MarkChunksIntersectingStamp(data, { stamp.m_x, stamp.m_z }, stamp.m_radius, margin);
			}
		}
	}
}

void LandscapeData::SetSettings(uint32_t chunksX,
	uint32_t chunksZ,
	float chunkSize,
	uint32_t chunkResolution,
	float heightScale,
	float noiseScale,
	uint32_t seed,
	float textureTiling)
{
	const uint32_t normalizedChunksX = (std::clamp)(chunksX, 1u, 64u);
	const uint32_t normalizedChunksZ = (std::clamp)(chunksZ, 1u, 64u);
	const float normalizedChunkSize = (std::max)(chunkSize, 1.0f);
	const uint32_t normalizedChunkResolution = (std::clamp)(chunkResolution, 2u, 128u);
	const float normalizedHeightScale = (std::max)(heightScale, 0.0f);
	const float normalizedNoiseScale = (std::max)(noiseScale, 0.0001f);
	const float normalizedTextureTiling = (std::max)(textureTiling, 0.001f);
	if (m_chunksX == normalizedChunksX && m_chunksZ == normalizedChunksZ && m_chunkSize == normalizedChunkSize &&
		m_chunkResolution == normalizedChunkResolution && m_heightScale == normalizedHeightScale &&
		m_noiseScale == normalizedNoiseScale && m_seed == seed && m_textureTiling == normalizedTextureTiling)
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
	MarkDirty();
}

void LandscapeData::SetLodSettings(const TVector<float>& distances, float skirtDepth)
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
	const float normalizedSkirtDepth = std::isfinite(skirtDepth) ? (std::clamp)(skirtDepth, 0.0f, 64.0f) : 2.0f;
	if (m_lodDistances == normalizedDistances && m_lodSkirtDepth == normalizedSkirtDepth)
	{
		return;
	}
	m_lodDistances = std::move(normalizedDistances);
	m_lodSkirtDepth = normalizedSkirtDepth;
	RequestFullRebuild();
}

void LandscapeData::SetGrassResidencyHysteresis(float grassResidencyHysteresis)
{
	const float normalizedHysteresis =
		std::isfinite(grassResidencyHysteresis) ? (std::clamp)(grassResidencyHysteresis, 0.0f, 512.0f) : 12.0f;
	if (m_grassResidencyHysteresis == normalizedHysteresis)
	{
		return;
	}
	m_grassResidencyHysteresis = normalizedHysteresis;
}

void LandscapeData::SetLayerTextures(const TVector<FileId>& textures)
{
	TVector<FileId> normalized = textures;
	if (normalized.Num() > 4u)
		normalized.Resize(4u);
	if (m_layerTextures == normalized)
	{
		return;
	}
	TVector<Tasks::TaskPtr<TexturePtr>> loads;
	loads.Resize(normalized.Num());
	for (size_t index = 0; index < normalized.Num() && index < m_layerTextureLoads.Num(); ++index)
	{
		if (normalized[index] == m_layerTextures[index]) loads[index] = m_layerTextureLoads[index];
	}
	m_layerTextures = std::move(normalized);
	m_layerTextureLoads = std::move(loads);
	m_runtimeMaterial.Clear();
	m_cachedSourceMaterialContentRevision = 0ull;
	m_cachedSourceMaterialRenderMetadataRevision = 0ull;
	MarkDirty();
}

void LandscapeData::SetImportMaps(const FileId& heightmapTexture, const TVector<FileId>& materialMasks)
{
	TVector<FileId> normalizedMasks = materialMasks;
	if (normalizedMasks.Num() > 4u)
		normalizedMasks.Resize(4u);
	if (m_heightmapTexture == heightmapTexture && m_materialMasks == normalizedMasks)
	{
		return;
	}
	m_heightmapTexture = heightmapTexture;
	m_materialMasks = std::move(normalizedMasks);
	m_importMapLoads.Clear();
	RequestFullRebuild();
}

void LandscapeData::SetAuthoredStamps(const TVector<LandscapeSculptStamp>& sculptStamps,
	const TVector<LandscapePaintStamp>& paintStamps)
{
	if (m_sculptStamps == sculptStamps && m_paintStamps == paintStamps)
	{
		return;
	}

	if (!m_bRebuildAllChunks && m_chunks.Num() == static_cast<size_t>(m_chunksX) * m_chunksZ)
	{
		const float normalSampleMargin = m_chunkSize / static_cast<float>((std::max)(m_chunkResolution, 1u));
		MarkChunksAffectedByStampChanges(*this, m_sculptStamps, sculptStamps, normalSampleMargin);
		MarkChunksAffectedByStampChanges(*this, m_paintStamps, paintStamps, 0.0f);
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
	for (uint32_t index = 0; index < m_vegetationProfiles.Num(); ++index)
	{
		auto& profile = m_vegetationProfiles[index];
		m_dirtyVegetationProfiles.Insert(index);
		m_bIsVegetationCollisionDirty |= profile.m_settings.HasCollision();
		if (!profile.m_model) profile.m_modelLoad.Clear();
		if (!profile.m_material) profile.m_materialLoad.Clear();
		profile.m_modelMaterialsLoad.Clear();
		profile.m_loadingModelMaterials.Clear();
		profile.m_bAreModelMaterialsPublished = false;
	}
	for (auto& load : m_layerTextureLoads)
	{
		if (load && load->IsFinished() && !load->GetResult())
		{
			load.Clear();
			m_runtimeMaterial.Clear();
		}
	}
	MarkDirty();
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

void LandscapeVegetationSettings::Normalize()
{
	auto finite = [](float value, float fallback) { return std::isfinite(value) ? value : fallback; };
	m_meshIndex = (std::clamp)(m_meshIndex, -1, 65535);
	m_instancesPerChunk = (std::min)(m_instancesPerChunk, 2048u);
	m_priority = (std::clamp)(finite(m_priority, 1.0f), 0.0f, 100.0f);
	m_minScale = (std::max)(finite(m_minScale, 0.75f), 0.01f);
	m_maxScale = (std::max)(finite(m_maxScale, 1.25f), m_minScale);
	m_groundOffset = finite(m_groundOffset, 0.0f);
	m_shadowDistance = (std::max)(finite(m_shadowDistance, 35.0f), 0.1f);
	m_minLod = (std::min)(m_minLod, 15u);
	m_maxLod = (std::clamp)(m_maxLod, m_minLod, 15u);
	for (size_t index = 0; index < m_screenCoverageThresholds.Num(); ++index)
	{
		const float fallback = index == 0 ? 0.25f : 0.05f;
		m_screenCoverageThresholds[index] = (std::clamp)(finite(m_screenCoverageThresholds[index], fallback), 0.0f, 1.0f);
	}
	std::sort(m_screenCoverageThresholds.begin(), m_screenCoverageThresholds.end(), std::greater<float>());
	m_cullDistance = (std::max)(finite(m_cullDistance, 120.0f), 0.1f);
	m_colliderRadius = (std::max)(finite(m_colliderRadius, 0.0f), 0.0f);
	m_colliderHeight = (std::max)(finite(m_colliderHeight, 2.0f), m_colliderRadius * 2.0f);
	m_colliderOffsetY = finite(m_colliderOffsetY, 1.0f);
}

void LandscapeData::SetVegetationProfiles(const TVector<LandscapeVegetationSettings>& settings)
{
	auto matchesCurrent = [&](const TVector<LandscapeVegetationSettings>& values)
	{
		return values.Num() == m_vegetationProfiles.Num() && std::equal(values.begin(), values.end(),
			m_vegetationProfiles.begin(), [](const auto& value, const auto& profile) { return value == profile.m_settings; });
	};
	if (matchesCurrent(settings)) return;

	TVector<LandscapeVegetationSettings> normalized = settings;
	for (auto& value : normalized) value.Normalize();
	if (matchesCurrent(normalized)) return;

	TVector<LandscapeVegetationProfile> profiles;
	profiles.Resize(normalized.Num());
	for (size_t index = 0; index < profiles.Num(); ++index)
	{
		auto& profile = profiles[index];
		profile.m_settings = std::move(normalized[index]);
		// Prefer the current slot, then reuse a matching resource after reorder/removal.
		auto findResource = [&](auto member) -> const LandscapeVegetationProfile*
		{
			auto matches = [&](const auto& previous) { return previous.m_settings.*member == profile.m_settings.*member; };
			if (index < m_vegetationProfiles.Num() && matches(m_vegetationProfiles[index])) return &m_vegetationProfiles[index];
			const size_t previous = m_vegetationProfiles.FindIf(matches);
			return previous != size_t(-1) ? &m_vegetationProfiles[previous] : nullptr;
		};
		if (const auto* previous = findResource(&LandscapeVegetationSettings::m_modelFileId))
		{
			profile.m_model = previous->m_model;
			profile.m_modelLoad = previous->m_modelLoad;
			profile.m_modelMaterials = previous->m_modelMaterials;
			profile.m_modelMaterialsLoad = previous->m_modelMaterialsLoad;
			profile.m_loadingModelMaterials = previous->m_loadingModelMaterials;
			profile.m_bAreModelMaterialsPublished = previous->m_bAreModelMaterialsPublished;
		}
		if (const auto* previous = findResource(&LandscapeVegetationSettings::m_materialFileId))
		{
			profile.m_material = previous->m_material;
			profile.m_materialLoad = previous->m_materialLoad;
		}
		// This revision describes the render proxy in this slot, not the reused model/material.
		if (index < m_vegetationProfiles.Num())
			profile.m_cachedRenderRevision = m_vegetationProfiles[index].m_cachedRenderRevision;
	}
	for (uint32_t index = 0; index < (std::max)(profiles.Num(), m_vegetationProfiles.Num()); ++index)
	{
		if (index >= profiles.Num() || index >= m_vegetationProfiles.Num())
		{
			m_dirtyVegetationProfiles.Insert(index);
			const auto& profile = index < profiles.Num() ? profiles[index] : m_vegetationProfiles[index];
			m_bIsVegetationCollisionDirty |= profile.m_settings.HasCollision();
			continue;
		}
		const auto& profile = profiles[index].m_settings;
		const auto& previous = m_vegetationProfiles[index].m_settings;
		const bool bPlacementChanged = static_cast<bool>(profile.m_modelFileId) != static_cast<bool>(previous.m_modelFileId) ||
			profile.m_residency != previous.m_residency || profile.m_instancesPerChunk != previous.m_instancesPerChunk ||
			profile.m_minScale != previous.m_minScale || profile.m_maxScale != previous.m_maxScale ||
			profile.m_groundOffset != previous.m_groundOffset;
		if (bPlacementChanged) m_dirtyVegetationProfiles.Insert(index);
		m_bIsVegetationCollisionDirty |= profile.HasCollision() != previous.HasCollision() ||
			(profile.HasCollision() && (bPlacementChanged || profile.m_colliderRadius != previous.m_colliderRadius ||
				profile.m_colliderHeight != previous.m_colliderHeight || profile.m_colliderOffsetY != previous.m_colliderOffsetY));
	}
	m_vegetationProfiles = std::move(profiles);
	MarkDirty();
}
