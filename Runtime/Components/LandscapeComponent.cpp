#include "Components/LandscapeComponent.h"
#include "Engine/GameObject.h"

#include <algorithm>

using namespace Sailor;

void LandscapeComponent::Initialize()
{
	auto* ecs = GetOwner()->GetWorld()->GetECS<LandscapeECS>();
	m_handle = ecs->RegisterComponent();
	auto& data = ecs->GetComponentData(m_handle);
	data.SetOwner(GetOwner());
	data.SetSettings(m_chunksX, m_chunksZ, m_chunkSize, m_chunkResolution,
		m_heightScale, m_noiseScale, m_seed, m_textureTiling);
	data.SetMaterial(m_material);
	data.SetLayerTextures(m_layerTextures);
	data.SetImportMaps(m_heightmapTexture, m_materialMasks);
	data.SetAuthoredStamps(m_sculptStamps, m_paintStamps);
	data.SetVegetationProfiles(m_vegetationModels, m_vegetationMaterials,
		m_vegetationMeshIndex, m_vegetationInstancesPerChunk, m_vegetationMinScale,
		m_vegetationMaxScale, m_vegetationGroundOffset,
		m_vegetationShadowMode, m_vegetationShadowDistance,
		m_vegetationMinLod, m_vegetationMaxLod,
		m_vegetationLod1ScreenCoverage, m_vegetationLod2ScreenCoverage,
		m_vegetationCullDistance, m_vegetationColliderRadius,
		m_vegetationColliderHeight, m_vegetationColliderOffsetY);
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
		data->SetMaterial(m_material);
		data->SetLayerTextures(m_layerTextures);
		data->SetImportMaps(m_heightmapTexture, m_materialMasks);
		data->SetAuthoredStamps(m_sculptStamps, m_paintStamps);
		data->SetVegetationProfiles(m_vegetationModels, m_vegetationMaterials,
			m_vegetationMeshIndex, m_vegetationInstancesPerChunk, m_vegetationMinScale,
			m_vegetationMaxScale, m_vegetationGroundOffset,
			m_vegetationShadowMode, m_vegetationShadowDistance,
			m_vegetationMinLod, m_vegetationMaxLod,
			m_vegetationLod1ScreenCoverage, m_vegetationLod2ScreenCoverage,
			m_vegetationCullDistance, m_vegetationColliderRadius,
			m_vegetationColliderHeight, m_vegetationColliderOffsetY);
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
void LandscapeComponent::SetSculptStamps(const TVector<float>& value) { m_sculptStamps = value; m_sculptStamps.Resize(m_sculptStamps.Num() / 5u * 5u); MarkDirty(); }
void LandscapeComponent::SetPaintStamps(const TVector<float>& value) { m_paintStamps = value; m_paintStamps.Resize(m_paintStamps.Num() / 5u * 5u); MarkDirty(); }
void LandscapeComponent::SetVegetationModels(const TVector<FileId>& value) { m_vegetationModels = value; MarkDirty(); }
void LandscapeComponent::SetVegetationMaterials(const TVector<FileId>& value) { m_vegetationMaterials = value; MarkDirty(); }
void LandscapeComponent::SetVegetationMeshIndex(const TVector<float>& value) { m_vegetationMeshIndex = value; MarkDirty(); }
void LandscapeComponent::SetVegetationInstancesPerChunk(const TVector<float>& value) { m_vegetationInstancesPerChunk = value; MarkDirty(); }
void LandscapeComponent::SetVegetationMinScale(const TVector<float>& value) { m_vegetationMinScale = value; MarkDirty(); }
void LandscapeComponent::SetVegetationMaxScale(const TVector<float>& value) { m_vegetationMaxScale = value; MarkDirty(); }
void LandscapeComponent::SetVegetationGroundOffset(const TVector<float>& value) { m_vegetationGroundOffset = value; MarkDirty(); }
void LandscapeComponent::SetVegetationShadowMode(const TVector<float>& value) { m_vegetationShadowMode = value; MarkDirty(); }
void LandscapeComponent::SetVegetationShadowDistance(const TVector<float>& value) { m_vegetationShadowDistance = value; MarkDirty(); }
void LandscapeComponent::SetVegetationMinLod(const TVector<float>& value) { m_vegetationMinLod = value; MarkDirty(); }
void LandscapeComponent::SetVegetationMaxLod(const TVector<float>& value) { m_vegetationMaxLod = value; MarkDirty(); }
void LandscapeComponent::SetVegetationLod1ScreenCoverage(const TVector<float>& value) { m_vegetationLod1ScreenCoverage = value; MarkDirty(); }
void LandscapeComponent::SetVegetationLod2ScreenCoverage(const TVector<float>& value) { m_vegetationLod2ScreenCoverage = value; MarkDirty(); }
void LandscapeComponent::SetVegetationCullDistance(const TVector<float>& value) { m_vegetationCullDistance = value; MarkDirty(); }
void LandscapeComponent::SetVegetationColliderRadius(const TVector<float>& value) { m_vegetationColliderRadius = value; MarkDirty(); }
void LandscapeComponent::SetVegetationColliderHeight(const TVector<float>& value) { m_vegetationColliderHeight = value; MarkDirty(); }
void LandscapeComponent::SetVegetationColliderOffsetY(const TVector<float>& value) { m_vegetationColliderOffsetY = value; MarkDirty(); }
void LandscapeComponent::SetRegenerate(bool value) { m_bRegenerate = false; if (value) MarkDirty(); }
void LandscapeComponent::SetFlatten(bool value) { m_bFlatten = false; if (value) { m_heightScale = 0.0f; MarkDirty(); } }
