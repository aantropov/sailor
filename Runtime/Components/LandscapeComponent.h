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
		SAILOR_API const TVector<LandscapeSculptStamp>& GetSculptStamps() const { return m_sculptStamps; }
		SAILOR_API void SetSculptStamps(const TVector<LandscapeSculptStamp>& value);
		SAILOR_API const TVector<LandscapePaintStamp>& GetPaintStamps() const { return m_paintStamps; }
		SAILOR_API void SetPaintStamps(const TVector<LandscapePaintStamp>& value);
		SAILOR_API const FileId& GetVegetation() const { return m_vegetation; }
		SAILOR_API void SetVegetation(const FileId& value);
		SAILOR_API const TVector<LandscapeVegetationSettings>& GetVegetationProfiles() const { return m_vegetationProfiles; }
		SAILOR_API void SetVegetationProfiles(const TVector<LandscapeVegetationSettings>& value);
		SAILOR_API bool GetRegenerate() const { return m_bRegenerate; }
		SAILOR_API void SetRegenerate(bool value);
		SAILOR_API bool GetFlatten() const { return m_bFlatten; }
		SAILOR_API void SetFlatten(bool value);
		SAILOR_API bool GetSaveVegetation() const { return false; }
		SAILOR_API void SetSaveVegetation(bool value);

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
		TVector<LandscapeSculptStamp> m_sculptStamps{};
		TVector<LandscapePaintStamp> m_paintStamps{};
		FileId m_vegetation{};
		TVector<LandscapeVegetationSettings> m_vegetationProfiles{};
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
	func(GetVegetation, property("vegetation")),
	func(SetVegetation, property("vegetation")),
	func(GetVegetationProfiles, property("vegetationProfiles")),
	func(SetVegetationProfiles, property("vegetationProfiles")),
	func(GetRegenerate, property("regenerate")),
	func(SetRegenerate, property("regenerate")),
	func(GetFlatten, property("flatten")),
	func(SetFlatten, property("flatten")),
	func(GetSaveVegetation, property("saveVegetation")),
	func(SetSaveVegetation, property("saveVegetation"))
)
