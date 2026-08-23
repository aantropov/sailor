#pragma once

#include "Components/Component.h"
#include "ECS/LandscapeECS.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"

namespace Sailor
{
	class LandscapeComponent final : public Component
	{
		SAILOR_REFLECTABLE(LandscapeComponent)

	public:
		SAILOR_API virtual void Initialize() override;
		SAILOR_API virtual void EndPlay() override;

		SAILOR_API uint32_t GetChunksX() const { return m_chunksX; }
		SAILOR_API void SetChunksX(uint32_t value);
		SAILOR_API uint32_t GetChunksZ() const { return m_chunksZ; }
		SAILOR_API void SetChunksZ(uint32_t value);
		SAILOR_API float GetChunkSize() const { return m_chunkSize; }
		SAILOR_API void SetChunkSize(float value);
		SAILOR_API uint32_t GetChunkResolution() const { return m_chunkResolution; }
		SAILOR_API void SetChunkResolution(uint32_t value);
		SAILOR_API float GetHeightScale() const { return m_heightScale; }
		SAILOR_API void SetHeightScale(float value);
		SAILOR_API float GetNoiseScale() const { return m_noiseScale; }
		SAILOR_API void SetNoiseScale(float value);
		SAILOR_API uint32_t GetSeed() const { return m_seed; }
		SAILOR_API void SetSeed(uint32_t value);
		SAILOR_API const MaterialPtr& GetMaterial() const { return m_material; }
		SAILOR_API void SetMaterial(const MaterialPtr& value);
		SAILOR_API const TVector<FileId>& GetLayerTextures() const { return m_layerTextures; }
		SAILOR_API void SetLayerTextures(const TVector<FileId>& value);
		SAILOR_API const FileId& GetHeightmapTexture() const { return m_heightmapTexture; }
		SAILOR_API void SetHeightmapTexture(const FileId& value);
		SAILOR_API const TVector<FileId>& GetMaterialMasks() const { return m_materialMasks; }
		SAILOR_API void SetMaterialMasks(const TVector<FileId>& value);
		SAILOR_API float GetTextureTiling() const { return m_textureTiling; }
		SAILOR_API void SetTextureTiling(float value);
		SAILOR_API const TVector<float>& GetLodDistances() const { return m_lodDistances; }
		SAILOR_API void SetLodDistances(const TVector<float>& value);
		SAILOR_API float GetLodSkirtDepth() const { return m_lodSkirtDepth; }
		SAILOR_API void SetLodSkirtDepth(float value);
		SAILOR_API float GetGrassResidencyHysteresis() const { return m_grassResidencyHysteresis; }
		SAILOR_API void SetGrassResidencyHysteresis(float value);
		SAILOR_API const TVector<float>& GetSculptStamps() const { return m_sculptStamps; }
		SAILOR_API void SetSculptStamps(const TVector<float>& value);
		SAILOR_API const TVector<float>& GetPaintStamps() const { return m_paintStamps; }
		SAILOR_API void SetPaintStamps(const TVector<float>& value);
		SAILOR_API const TVector<FileId>& GetVegetationModels() const { return m_vegetationModels; }
		SAILOR_API void SetVegetationModels(const TVector<FileId>& value);
		SAILOR_API const TVector<FileId>& GetVegetationMaterials() const { return m_vegetationMaterials; }
		SAILOR_API void SetVegetationMaterials(const TVector<FileId>& value);
		SAILOR_API const TVector<float>& GetVegetationMeshIndex() const { return m_vegetationMeshIndex; }
		SAILOR_API void SetVegetationMeshIndex(const TVector<float>& value);
		SAILOR_API const TVector<float>& GetVegetationInstancesPerChunk() const { return m_vegetationInstancesPerChunk; }
		SAILOR_API void SetVegetationInstancesPerChunk(const TVector<float>& value);
		SAILOR_API const TVector<float>& GetVegetationResidency() const { return m_vegetationResidency; }
		SAILOR_API void SetVegetationResidency(const TVector<float>& value);
		SAILOR_API const TVector<float>& GetVegetationPriority() const { return m_vegetationPriority; }
		SAILOR_API void SetVegetationPriority(const TVector<float>& value);
		SAILOR_API const TVector<float>& GetVegetationMinScale() const { return m_vegetationMinScale; }
		SAILOR_API void SetVegetationMinScale(const TVector<float>& value);
		SAILOR_API const TVector<float>& GetVegetationMaxScale() const { return m_vegetationMaxScale; }
		SAILOR_API void SetVegetationMaxScale(const TVector<float>& value);
		SAILOR_API const TVector<float>& GetVegetationGroundOffset() const { return m_vegetationGroundOffset; }
		SAILOR_API void SetVegetationGroundOffset(const TVector<float>& value);
		SAILOR_API const TVector<float>& GetVegetationShadowMode() const { return m_vegetationShadowMode; }
		SAILOR_API void SetVegetationShadowMode(const TVector<float>& value);
		SAILOR_API const TVector<float>& GetVegetationShadowDistance() const { return m_vegetationShadowDistance; }
		SAILOR_API void SetVegetationShadowDistance(const TVector<float>& value);
		SAILOR_API const TVector<float>& GetVegetationMinLod() const { return m_vegetationMinLod; }
		SAILOR_API void SetVegetationMinLod(const TVector<float>& value);
		SAILOR_API const TVector<float>& GetVegetationMaxLod() const { return m_vegetationMaxLod; }
		SAILOR_API void SetVegetationMaxLod(const TVector<float>& value);
		SAILOR_API const TVector<float>& GetVegetationLod1ScreenCoverage() const { return m_vegetationLod1ScreenCoverage; }
		SAILOR_API void SetVegetationLod1ScreenCoverage(const TVector<float>& value);
		SAILOR_API const TVector<float>& GetVegetationLod2ScreenCoverage() const { return m_vegetationLod2ScreenCoverage; }
		SAILOR_API void SetVegetationLod2ScreenCoverage(const TVector<float>& value);
		SAILOR_API const TVector<float>& GetVegetationCullDistance() const { return m_vegetationCullDistance; }
		SAILOR_API void SetVegetationCullDistance(const TVector<float>& value);
		SAILOR_API const TVector<float>& GetVegetationColliderRadius() const { return m_vegetationColliderRadius; }
		SAILOR_API void SetVegetationColliderRadius(const TVector<float>& value);
		SAILOR_API const TVector<float>& GetVegetationColliderHeight() const { return m_vegetationColliderHeight; }
		SAILOR_API void SetVegetationColliderHeight(const TVector<float>& value);
		SAILOR_API const TVector<float>& GetVegetationColliderOffsetY() const { return m_vegetationColliderOffsetY; }
		SAILOR_API void SetVegetationColliderOffsetY(const TVector<float>& value);
		SAILOR_API bool GetRegenerate() const { return m_bRegenerate; }
		SAILOR_API void SetRegenerate(bool value);
		SAILOR_API bool GetFlatten() const { return m_bFlatten; }
		SAILOR_API void SetFlatten(bool value);

		SAILOR_API size_t GetComponentIndex() const { return m_handle; }

	private:
		void MarkDirty();
		LandscapeData* TryGetData();

		size_t m_handle = ECS::InvalidIndex;
		uint32_t m_chunksX = 4u;
		uint32_t m_chunksZ = 4u;
		float m_chunkSize = 24.0f;
		uint32_t m_chunkResolution = 24u;
		float m_heightScale = 5.0f;
		float m_noiseScale = 0.035f;
		uint32_t m_seed = 1337u;
		MaterialPtr m_material{};
		TVector<FileId> m_layerTextures{};
		FileId m_heightmapTexture{};
		TVector<FileId> m_materialMasks{};
		float m_textureTiling = 0.15f;
		TVector<float> m_lodDistances{ 96.0f, 192.0f };
		float m_lodSkirtDepth = 2.0f;
		float m_grassResidencyHysteresis = 12.0f;
		TVector<float> m_sculptStamps{};
		TVector<float> m_paintStamps{};
		TVector<FileId> m_vegetationModels{};
		TVector<FileId> m_vegetationMaterials{};
		TVector<float> m_vegetationMeshIndex{};
		TVector<float> m_vegetationInstancesPerChunk{};
		TVector<float> m_vegetationResidency{};
		TVector<float> m_vegetationPriority{};
		TVector<float> m_vegetationMinScale{};
		TVector<float> m_vegetationMaxScale{};
		TVector<float> m_vegetationGroundOffset{};
		TVector<float> m_vegetationShadowMode{};
		TVector<float> m_vegetationShadowDistance{};
		TVector<float> m_vegetationMinLod{};
		TVector<float> m_vegetationMaxLod{};
		TVector<float> m_vegetationLod1ScreenCoverage{};
		TVector<float> m_vegetationLod2ScreenCoverage{};
		TVector<float> m_vegetationCullDistance{};
		TVector<float> m_vegetationColliderRadius{};
		TVector<float> m_vegetationColliderHeight{};
		TVector<float> m_vegetationColliderOffsetY{};
		bool m_bRegenerate = false;
		bool m_bFlatten = false;
	};
}

using namespace Sailor::Attributes;

REFL_AUTO(
	type(Sailor::LandscapeComponent, bases<Sailor::Component>),
	func(GetChunksX, property("chunksX"), Range(1.0, 64.0)),
	func(SetChunksX, property("chunksX")),
	func(GetChunksZ, property("chunksZ"), Range(1.0, 64.0)),
	func(SetChunksZ, property("chunksZ")),
	func(GetChunkSize, property("chunkSize"), Range(1.0, 512.0)),
	func(SetChunkSize, property("chunkSize")),
	func(GetChunkResolution, property("chunkResolution"), Range(2.0, 128.0)),
	func(SetChunkResolution, property("chunkResolution")),
	func(GetHeightScale, property("heightScale"), Range(0.0, 128.0)),
	func(SetHeightScale, property("heightScale")),
	func(GetNoiseScale, property("noiseScale"), Range(0.0001, 2.0)),
	func(SetNoiseScale, property("noiseScale")),
	func(GetSeed, property("seed")),
	func(SetSeed, property("seed")),
	func(GetMaterial, property("material"), SkipCDO()),
	func(SetMaterial, property("material"), SkipCDO()),
	func(GetLayerTextures, property("layerTextures")),
	func(SetLayerTextures, property("layerTextures")),
	func(GetHeightmapTexture, property("heightmapTexture")),
	func(SetHeightmapTexture, property("heightmapTexture")),
	func(GetMaterialMasks, property("materialMasks")),
	func(SetMaterialMasks, property("materialMasks")),
	func(GetTextureTiling, property("textureTiling"), Range(0.001, 8.0)),
	func(SetTextureTiling, property("textureTiling")),
	func(GetLodDistances, property("lodDistances")),
	func(SetLodDistances, property("lodDistances")),
	func(GetLodSkirtDepth, property("lodSkirtDepth"), Range(0.0, 64.0)),
	func(SetLodSkirtDepth, property("lodSkirtDepth")),
	func(GetGrassResidencyHysteresis, property("grassResidencyHysteresis"), Range(0.0, 512.0)),
	func(SetGrassResidencyHysteresis, property("grassResidencyHysteresis")),
	func(GetSculptStamps, property("sculptStamps")),
	func(SetSculptStamps, property("sculptStamps")),
	func(GetPaintStamps, property("paintStamps")),
	func(SetPaintStamps, property("paintStamps")),
	func(GetVegetationModels, property("vegetationModels")),
	func(SetVegetationModels, property("vegetationModels")),
	func(GetVegetationMaterials, property("vegetationMaterials")),
	func(SetVegetationMaterials, property("vegetationMaterials")),
	func(GetVegetationMeshIndex, property("vegetationMeshIndex")),
	func(SetVegetationMeshIndex, property("vegetationMeshIndex")),
	func(GetVegetationInstancesPerChunk, property("vegetationInstancesPerChunk")),
	func(SetVegetationInstancesPerChunk, property("vegetationInstancesPerChunk")),
	func(GetVegetationResidency, property("vegetationResidency")),
	func(SetVegetationResidency, property("vegetationResidency")),
	func(GetVegetationPriority, property("vegetationPriority")),
	func(SetVegetationPriority, property("vegetationPriority")),
	func(GetVegetationMinScale, property("vegetationMinScale")),
	func(SetVegetationMinScale, property("vegetationMinScale")),
	func(GetVegetationMaxScale, property("vegetationMaxScale")),
	func(SetVegetationMaxScale, property("vegetationMaxScale")),
	func(GetVegetationGroundOffset, property("vegetationGroundOffset")),
	func(SetVegetationGroundOffset, property("vegetationGroundOffset")),
	func(GetVegetationShadowMode, property("vegetationShadowMode")),
	func(SetVegetationShadowMode, property("vegetationShadowMode")),
	func(GetVegetationShadowDistance, property("vegetationShadowDistance")),
	func(SetVegetationShadowDistance, property("vegetationShadowDistance")),
	func(GetVegetationMinLod, property("vegetationMinLod")),
	func(SetVegetationMinLod, property("vegetationMinLod")),
	func(GetVegetationMaxLod, property("vegetationMaxLod")),
	func(SetVegetationMaxLod, property("vegetationMaxLod")),
	func(GetVegetationLod1ScreenCoverage, property("vegetationLod1ScreenCoverage")),
	func(SetVegetationLod1ScreenCoverage, property("vegetationLod1ScreenCoverage")),
	func(GetVegetationLod2ScreenCoverage, property("vegetationLod2ScreenCoverage")),
	func(SetVegetationLod2ScreenCoverage, property("vegetationLod2ScreenCoverage")),
	func(GetVegetationCullDistance, property("vegetationCullDistance")),
	func(SetVegetationCullDistance, property("vegetationCullDistance")),
	func(GetVegetationColliderRadius, property("vegetationColliderRadius")),
	func(SetVegetationColliderRadius, property("vegetationColliderRadius")),
	func(GetVegetationColliderHeight, property("vegetationColliderHeight")),
	func(SetVegetationColliderHeight, property("vegetationColliderHeight")),
	func(GetVegetationColliderOffsetY, property("vegetationColliderOffsetY")),
	func(SetVegetationColliderOffsetY, property("vegetationColliderOffsetY")),
	func(GetRegenerate, property("regenerate")),
	func(SetRegenerate, property("regenerate")),
	func(GetFlatten, property("flatten")),
	func(SetFlatten, property("flatten"))
)
