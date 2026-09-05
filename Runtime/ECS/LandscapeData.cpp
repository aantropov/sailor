#include "ECS/LandscapeECS.h"
#include "ECS/LandscapeECSInternal.h"

#include <algorithm>
#include <cmath>

using namespace Sailor;
using namespace Sailor::LandscapeECSInternal;

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
	RequestFullRebuild();
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
	m_layerTextures = std::move(normalized);
	m_runtimeMaterial.Clear();
	m_cachedSourceMaterialContentRevision = 0ull;
	m_cachedSourceMaterialRenderMetadataRevision = 0ull;
	RequestFullRebuild();
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
	RequestFullRebuild();
}

void LandscapeData::SetAuthoredStamps(const TVector<float>& sculptStamps, const TVector<float>& paintStamps)
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

void LandscapeData::SetVegetationProfiles(const TVector<FileId>& models,
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
		profile.m_meshIndex =
			static_cast<int32_t>((std::clamp)(GetProfileValue(meshIndex, index, -1.0f), -1.0f, 65535.0f));
		profile.m_instancesPerChunk =
			static_cast<uint32_t>((std::clamp)(GetProfileValue(instancesPerChunk, index, 0.0f), 0.0f, 2048.0f));
		profile.m_residency = static_cast<ELandscapeVegetationResidency>(
			static_cast<uint32_t>((std::clamp)(GetProfileValue(residency, index, 0.0f), 0.0f, 1.0f)));
		profile.m_priority = (std::clamp)(GetProfileValue(priority, index, 1.0f), 0.0f, 100.0f);
		profile.m_minScale = (std::max)(GetProfileValue(minScale, index, 0.75f), 0.01f);
		profile.m_maxScale = (std::max)(GetProfileValue(maxScale, index, 1.25f), profile.m_minScale);
		profile.m_groundOffset = GetProfileValue(groundOffset, index, 0.0f);
		profile.m_shadowMode = static_cast<ELandscapeVegetationShadowMode>(
			static_cast<uint32_t>((std::clamp)(GetProfileValue(shadowMode, index, 1.0f), 0.0f, 2.0f)));
		profile.m_shadowDistance = (std::max)(GetProfileValue(shadowDistance, index, 35.0f), 0.1f);
		profile.m_minLod = static_cast<uint32_t>((std::clamp)(GetProfileValue(minLod, index, 0.0f), 0.0f, 15.0f));
		profile.m_maxLod = static_cast<uint32_t>(
			(std::clamp)(GetProfileValue(maxLod, index, 2.0f), static_cast<float>(profile.m_minLod), 15.0f));
		profile.m_screenCoverageThresholds = {
			(std::clamp)(GetProfileValue(lod1ScreenCoverage, index, 0.25f), 0.0f, 1.0f),
			(std::clamp)(GetProfileValue(lod2ScreenCoverage, index, 0.05f), 0.0f, 1.0f)};
		std::sort(profile.m_screenCoverageThresholds.begin(),
			profile.m_screenCoverageThresholds.end(),
			std::greater<float>());
		profile.m_cullDistance = (std::max)(GetProfileValue(cullDistance, index, 120.0f), 0.1f);
		profile.m_colliderRadius = (std::max)(GetProfileValue(colliderRadius, index, 0.0f), 0.0f);
		profile.m_colliderHeight =
			(std::max)(GetProfileValue(colliderHeight, index, 2.0f), profile.m_colliderRadius * 2.0f);
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
		bSettingsChanged = !AreVegetationProfileSettingsEqual(profiles[index], m_vegetationProfiles[index]);
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
			profile.m_cachedMaterialRenderMetadataRevision = previous.m_cachedMaterialRenderMetadataRevision;
		}
	}
	m_vegetationProfiles = std::move(profiles);
	RequestFullRebuild();
}
