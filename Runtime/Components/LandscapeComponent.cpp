#include "Components/LandscapeComponent.h"
#include "Engine/GameObject.h"

#include <algorithm>
#include <cmath>

using namespace Sailor;

void LandscapeComponent::Initialize()
{
	auto* ecs = GetOwner()->GetWorld()->GetECS<LandscapeECS>();
	m_handle = ecs->RegisterComponent();
	auto& data = ecs->GetComponentData(m_handle);
	data.SetOwner(GetOwner());
	data.SetSettings(m_chunksX, m_chunksZ, m_chunkSize, m_chunkResolution,
		m_heightScale, m_noiseScale, m_seed, m_textureTiling);
	data.SetLodSettings(m_lodDistances, m_lodSkirtDepth);
	data.SetGrassResidencyHysteresis(m_grassResidencyHysteresis);
	data.SetMaterial(m_material);
	data.SetLayerTextures(m_layerTextures);
	data.SetImportMaps(m_heightmapTexture, m_materialMasks);
	data.SetAuthoredStamps(m_sculptStamps, m_paintStamps);
	data.SetVegetationAsset(m_vegetation);
	data.SetVegetationProfiles(m_vegetationProfiles);
}

void LandscapeComponent::EndPlay()
{
	if (m_handle != ECS::InvalidIndex)
	{
		GetOwner()->GetWorld()->GetECS<LandscapeECS>()->UnregisterComponent(m_handle);
		m_handle = ECS::InvalidIndex;
	}
}

LandscapeData* LandscapeComponent::TryGetData()
{
	if (m_handle == ECS::InvalidIndex || !GetOwner() || !GetOwner()->GetWorld())
	{
		return nullptr;
	}
	auto* ecs = GetOwner()->GetWorld()->GetECS<LandscapeECS>();
	return ecs && ecs->IsComponentRegistered(m_handle) ? &ecs->GetComponentData(m_handle) : nullptr;
}

void LandscapeComponent::MarkDirty()
{
	if (auto* data = TryGetData())
	{
		data->SetSettings(m_chunksX, m_chunksZ, m_chunkSize, m_chunkResolution,
			m_heightScale, m_noiseScale, m_seed, m_textureTiling);
		data->SetLodSettings(m_lodDistances, m_lodSkirtDepth);
		data->SetGrassResidencyHysteresis(m_grassResidencyHysteresis);
		data->SetMaterial(m_material);
		data->SetLayerTextures(m_layerTextures);
		data->SetImportMaps(m_heightmapTexture, m_materialMasks);
		data->SetAuthoredStamps(m_sculptStamps, m_paintStamps);
		data->SetVegetationAsset(m_vegetation);
		data->SetVegetationProfiles(m_vegetationProfiles);
	}
}

void LandscapeComponent::SetChunksX(uint32_t value) { m_chunksX = (std::clamp)(value, 1u, 64u); MarkDirty(); }
void LandscapeComponent::SetChunksZ(uint32_t value) { m_chunksZ = (std::clamp)(value, 1u, 64u); MarkDirty(); }
void LandscapeComponent::SetChunkSize(float value) { m_chunkSize = (std::max)(value, 1.0f); MarkDirty(); }
void LandscapeComponent::SetChunkResolution(uint32_t value) { m_chunkResolution = (std::clamp)(value, 2u, 128u); MarkDirty(); }
void LandscapeComponent::SetHeightScale(float value) { m_heightScale = (std::max)(value, 0.0f); MarkDirty(); }
void LandscapeComponent::SetNoiseScale(float value) { m_noiseScale = (std::max)(value, 0.0001f); MarkDirty(); }
void LandscapeComponent::SetSeed(uint32_t value) { m_seed = value; MarkDirty(); }
void LandscapeComponent::SetMaterial(const MaterialPtr& value) { m_material = value; MarkDirty(); }
void LandscapeComponent::SetLayerTextures(const TVector<FileId>& value) { m_layerTextures = value; if (m_layerTextures.Num() > 4u) m_layerTextures.Resize(4u); MarkDirty(); }
void LandscapeComponent::SetHeightmapTexture(const FileId& value) { m_heightmapTexture = value; MarkDirty(); }
void LandscapeComponent::SetMaterialMasks(const TVector<FileId>& value) { m_materialMasks = value; if (m_materialMasks.Num() > 4u) m_materialMasks.Resize(4u); MarkDirty(); }
void LandscapeComponent::SetTextureTiling(float value) { m_textureTiling = (std::max)(value, 0.001f); MarkDirty(); }
void LandscapeComponent::SetLodDistances(const TVector<float>& value)
{
	m_lodDistances = value;
	for (float& distance : m_lodDistances)
	{
		distance = std::isfinite(distance) ? (std::max)(distance, 1.0f) : 1.0f;
	}
	std::sort(m_lodDistances.begin(), m_lodDistances.end());
	m_lodDistances.Resize((std::min)(m_lodDistances.Num(), size_t(7u)));
	MarkDirty();
}
void LandscapeComponent::SetLodSkirtDepth(float value) { m_lodSkirtDepth = std::isfinite(value) ? (std::clamp)(value, 0.0f, 64.0f) : 2.0f; MarkDirty(); }
void LandscapeComponent::SetGrassResidencyHysteresis(float value) { m_grassResidencyHysteresis = std::isfinite(value) ? (std::clamp)(value, 0.0f, 512.0f) : 12.0f; MarkDirty(); }
void LandscapeComponent::SetSculptStamps(const TVector<LandscapeSculptStamp>& value) { m_sculptStamps = value; MarkDirty(); }
void LandscapeComponent::SetPaintStamps(const TVector<LandscapePaintStamp>& value) { m_paintStamps = value; MarkDirty(); }
void LandscapeComponent::SetVegetation(const FileId& value) { m_vegetation = value; MarkDirty(); }
void LandscapeComponent::SetVegetationProfiles(const TVector<LandscapeVegetationSettings>& value)
{
	if (m_vegetationProfiles == value) return;
	m_vegetationProfiles = value;
	for (auto& settings : m_vegetationProfiles) settings.Normalize();
	MarkDirty();
}
void LandscapeComponent::SetRegenerate(bool value)
{
	m_bRegenerate = false;
	if (value)
	{
		if (auto* data = TryGetData())
		{
			data->RequestVegetationAssetReload();
			data->RequestFullRebuild();
		}
	}
}
void LandscapeComponent::SetFlatten(bool value) { m_bFlatten = false; if (value) { m_heightScale = 0.0f; MarkDirty(); } }
void LandscapeComponent::SetSaveVegetation(bool value)
{
	if (value)
	{
		if (auto* data = TryGetData())
		{
			data->RequestSaveVegetation();
		}
	}
}
