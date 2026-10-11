#pragma once

#include "ECS/ECS.h"
#include "Landscape/LandscapeSettings.h"
#include "AssetRegistry/Landscape/LandscapeVegetationAsset.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Math/Bounds.h"
#include "RHI/SceneView.h"

#include <string>
#include <limits>

namespace Sailor
{
	constexpr EMobilityType ResolveLandscapeProxyMobility(
		EMobilityType ownerMobility,
		ELandscapeVegetationResidency residency) noexcept
	{
		return residency == ELandscapeVegetationResidency::Grass ?
			EMobilityType::Dynamic : ownerMobility;
	}

	struct LandscapeVegetationProfile final
	{
		LandscapeVegetationSettings m_settings{};
		ModelPtr m_model{};
		MaterialPtr m_material{};
		Tasks::TaskPtr<ModelPtr> m_modelLoad{};
		Tasks::TaskPtr<MaterialPtr> m_materialLoad{};
		Tasks::TaskPtr<bool> m_modelMaterialsLoad{};
		TVector<MaterialPtr> m_loadingModelMaterials{};
		TVector<MaterialPtr> m_modelMaterials{};
		bool m_bAreModelMaterialsPublished = false;
		uint64_t m_cachedRenderRevision = 0ull;
	};

	struct LandscapeVegetationRenderProxy final
	{
		RHI::RHISceneProxyResourcePtr m_resource{};
		Math::AABB m_localBounds{};
		glm::ivec3 m_octreeCenter{};
		glm::ivec3 m_octreeExtents{ 1 };
		size_t m_profileIndex = 0u;
		uint32_t m_instanceCount = 0u;
		uint64_t m_revision = 0u;
		uint64_t m_viewRevision = 0u;
		uint64_t m_renderRevision = 0u;
		ELandscapeVegetationResidency m_residency =
			ELandscapeVegetationResidency::Persistent;
		EMobilityType m_mobility = EMobilityType::Static;
	};

	constexpr bool IsLandscapeGrassProxy(
		const LandscapeVegetationRenderProxy& proxy) noexcept
	{
		return proxy.m_residency == ELandscapeVegetationResidency::Grass;
	}

	struct LandscapeVegetationRenderInstances final
	{
		TVector<glm::mat4> m_transforms{};
		TVector<int32_t> m_lodBiases{};
		TVector<float> m_cullDistanceScales{};
		TVector<float> m_shadowDistanceScales{};
	};

	struct LandscapePendingVegetation final
	{
		size_t m_chunkIndex = 0u;
		size_t m_profileIndex = 0u;
		LandscapeVegetationRenderInstances m_instances{};
	};

	struct LandscapeChunk final
	{
		RHI::RHISceneProxyResourcePtr m_resource{};
		glm::ivec3 m_octreeCenter{};
		glm::ivec3 m_octreeExtents{ 1 };
		TVector<LandscapeVegetationRenderProxy> m_vegetationProxies{};
		TVector<float> m_heightSamples{};
		uint32_t m_heightResolution = 0u;
		uint32_t m_chunkX = 0u;
		uint32_t m_chunkZ = 0u;
		uint32_t m_terrainBodyId = (std::numeric_limits<uint32_t>::max)();
		uint32_t m_vegetationBodyId = (std::numeric_limits<uint32_t>::max)();
		TSharedPtr<TVector<Math::Triangle>> m_bakeTriangles{};
		TVector<LandscapeVegetationInstance> m_bakeVegetation{};
		Math::AABB m_localBounds{};
		uint64_t m_buildRevision = 0u;
		uint64_t m_vegetationRevision = 0u;
	};

	struct LandscapeBakeGeometrySnapshot final
	{
		std::string m_sourceId{};
		ModelPtr m_model{};
		int32_t m_meshIndex = -1;
		TSharedPtr<TVector<Math::Triangle>> m_triangles{};
		glm::mat4 m_worldMatrix{ 1.0f };
		Math::AABB m_worldBounds{};
		TVector<MaterialPtr> m_materials{};
		uint64_t m_sourceRevision = 0u;
	};

	class LandscapeData final : public ECS::TComponent
	{
	public:
		SAILOR_API void SetSettings(uint32_t chunksX, uint32_t chunksZ,
			float chunkSize, uint32_t chunkResolution, float heightScale,
			float noiseScale, uint32_t seed, float textureTiling);
		SAILOR_API void SetLodSettings(
			const TVector<float>& distances,
			float skirtDepth);
		SAILOR_API void SetGrassResidencyHysteresis(
			float grassResidencyHysteresis);
		SAILOR_API void SetMaterial(const MaterialPtr& material);
		SAILOR_API void SetLayerTextures(const TVector<FileId>& textures);
		SAILOR_API void SetImportMaps(const FileId& heightmapTexture,
			const TVector<FileId>& materialMasks);
		SAILOR_API void SetAuthoredStamps(const TVector<LandscapeSculptStamp>& sculptStamps,
			const TVector<LandscapePaintStamp>& paintStamps);
		SAILOR_API void SetVegetationAsset(const FileId& vegetationAsset);
		SAILOR_API void RequestVegetationAssetReload();
		SAILOR_API void RequestSaveVegetation();
		SAILOR_API void RequestFullRebuild();
		SAILOR_API void SetVegetationProfiles(const TVector<LandscapeVegetationSettings>& settings);

	public:
		// Immutable while worker tasks build a dirty landscape revision.
		// LandscapeECS owns mutation and publishes the finished chunks atomically
		// on the game thread.
		uint32_t m_chunksX = 4u;
		uint32_t m_chunksZ = 4u;
		float m_chunkSize = 24.0f;
		uint32_t m_chunkResolution = 24u;
		float m_heightScale = 5.0f;
		float m_noiseScale = 0.035f;
		uint32_t m_seed = 1337u;
		float m_textureTiling = 0.15f;
		TVector<float> m_lodDistances{ 96.0f, 192.0f };
		float m_lodSkirtDepth = 2.0f;
		float m_grassResidencyHysteresis = 12.0f;
		MaterialPtr m_material{};
		MaterialPtr m_runtimeMaterial{};
		uint64_t m_cachedSourceMaterialContentRevision = 0ull;
		uint64_t m_cachedSourceMaterialRenderMetadataRevision = 0ull;
		TVector<FileId> m_layerTextures{};
		TVector<Tasks::TaskPtr<TexturePtr>> m_layerTextureLoads{};
		FileId m_heightmapTexture{};
		TVector<FileId> m_materialMasks{};
		TVector<Tasks::TaskPtr<TextureImporter::CpuTextureSnapshot>> m_importMapLoads{};
		TVector<LandscapeSculptStamp> m_sculptStamps{};
		TVector<LandscapePaintStamp> m_paintStamps{};
		FileId m_vegetationAsset{};
		LandscapeVegetationAssetData m_vegetationAssetData{};
		bool m_bVegetationAssetLoaded = false;
		bool m_bReloadVegetationAsset = false;
		bool m_bSaveVegetationRequested = false;
		TVector<LandscapeVegetationProfile> m_vegetationProfiles{};
		TVector<LandscapeChunk> m_chunks{};
		TVector<LandscapePendingVegetation> m_pendingVegetation{};
		TVector<uint32_t> m_physicsBodies{};
		TSet<uint32_t> m_dirtyChunks{};
		TSet<uint32_t> m_dirtyVegetationProfiles{};
		bool m_bIsVegetationCollisionDirty = false;
		glm::vec3 m_physicsScale{ 1.0f };
		bool m_bRebuildAllChunks = true;
		uint64_t m_buildRevision = 0u;
		uint64_t m_streamingRevision = 0u;
		uint64_t m_vegetationRevision = 0u;
		uint32_t m_activeGrassInstances = 0u;

		friend class LandscapeECS;
	};
}
