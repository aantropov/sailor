#pragma once

#include "ECS/ECS.h"
#include "ECS/LandscapeStreaming.h"
#include "AssetRegistry/Landscape/LandscapeVegetationAsset.h"
#include "Math/Bounds.h"
#include "RHI/SceneView.h"

#include <string>

namespace Sailor
{
	class CameraData;

	enum class ELandscapeVegetationShadowMode : uint8_t
	{
		None = 0u,
		NearOnly,
		All
	};

	enum class ELandscapeVegetationResidency : uint8_t
	{
		Persistent = 0u,
		Grass
	};

	constexpr EMobilityType ResolveLandscapeProxyMobility(
		EMobilityType ownerMobility,
		ELandscapeVegetationResidency residency) noexcept
	{
		return residency == ELandscapeVegetationResidency::Grass ?
			EMobilityType::Dynamic : ownerMobility;
	}

	struct LandscapeVegetationProfile final
	{
		FileId m_modelFileId{};
		FileId m_materialFileId{};
		ModelPtr m_model{};
		MaterialPtr m_material{};
		TVector<MaterialPtr> m_modelMaterials{};
		bool m_bModelMaterialsRequested = false;
		uint64_t m_cachedMaterialRenderMetadataRevision = 0ull;
		int32_t m_meshIndex = -1;
		uint32_t m_instancesPerChunk = 0u;
		ELandscapeVegetationResidency m_residency =
			ELandscapeVegetationResidency::Persistent;
		float m_priority = 1.0f;
		float m_minScale = 0.75f;
		float m_maxScale = 1.25f;
		float m_groundOffset = 0.0f;
		ELandscapeVegetationShadowMode m_shadowMode = ELandscapeVegetationShadowMode::NearOnly;
		float m_shadowDistance = 35.0f;
		uint32_t m_minLod = 0u;
		uint32_t m_maxLod = 2u;
		TVector<float> m_screenCoverageThresholds{ 0.25f, 0.05f };
		float m_cullDistance = 120.0f;
		float m_colliderRadius = 0.0f;
		float m_colliderHeight = 2.0f;
		float m_colliderOffsetY = 1.0f;
	};

	struct LandscapeVegetationRenderProxy final
	{
		RHI::RHISceneProxyResourcePtr m_resource{};
		glm::ivec3 m_octreeCenter{};
		glm::ivec3 m_octreeExtents{ 1 };
		size_t m_profileIndex = 0u;
		uint32_t m_instanceCount = 0u;
		uint64_t m_revision = 0u;
		uint64_t m_viewRevision = 0u;
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

	struct LandscapeBakeVegetationInstance final
	{
		size_t m_profileIndex = 0u;
		glm::mat4 m_localMatrix{ 1.0f };
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
		TVector<uint32_t> m_physicsBodies{};
		TSharedPtr<TVector<Math::Triangle>> m_bakeTriangles{};
		TVector<LandscapeBakeVegetationInstance> m_bakeVegetation{};
		Math::AABB m_localBounds{};
		uint64_t m_buildRevision = 0u;
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
		SAILOR_API void SetAuthoredStamps(const TVector<float>& sculptStamps,
			const TVector<float>& paintStamps);
		SAILOR_API void SetVegetationAsset(const FileId& vegetationAsset);
		SAILOR_API void RequestVegetationAssetReload();
		SAILOR_API void RequestSaveVegetation();
		SAILOR_API void RequestFullRebuild();
		SAILOR_API void SetVegetationProfiles(
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
			const TVector<float>& colliderOffsetY);

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
		FileId m_heightmapTexture{};
		TVector<FileId> m_materialMasks{};
		TVector<float> m_sculptStamps{};
		TVector<float> m_paintStamps{};
		FileId m_vegetationAsset{};
		LandscapeVegetationAssetData m_vegetationAssetData{};
		bool m_bVegetationAssetLoaded = false;
		bool m_bReloadVegetationAsset = false;
		bool m_bSaveVegetationRequested = false;
		TVector<LandscapeVegetationProfile> m_vegetationProfiles{};
		TVector<LandscapeChunk> m_chunks{};
		TVector<uint32_t> m_physicsBodies{};
		TSet<uint32_t> m_dirtyChunks{};
		bool m_bRebuildAllChunks = true;
		uint64_t m_buildRevision = 0u;
		uint64_t m_streamingRevision = 0u;
		uint32_t m_activeGrassInstances = 0u;

		friend class LandscapeECS;
	};

	class LandscapeECS final : public ECS::TSystem<LandscapeECS, LandscapeData>
	{
	public:
		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual Tasks::ITaskPtr Tick(float deltaTime) override;
		SAILOR_API virtual void EndPlay() override;
		SAILOR_API void MarkDirty(GameObjectPtr owner);
		SAILOR_API void AppendSceneView(RHI::RHISceneViewPtr& sceneView) const;
		SAILOR_API bool CollectBakeGeometrySnapshots(
			TVector<LandscapeBakeGeometrySnapshot>& outSnapshots,
			std::string& outDiagnostic) const;
		virtual uint32_t GetOrder() const override { return 990u; }

	protected:
		SAILOR_API virtual void OnComponentUnregistered(size_t index, LandscapeData& component) override;

	private:
		struct GrassTransformBuildRequest final
		{
			size_t m_componentIndex = 0u;
			size_t m_chunkIndex = 0u;
			size_t m_profileIndex = 0u;
			uint32_t m_instanceCount = 0u;
			uint64_t m_viewRevision = 0u;
			Tasks::TaskPtr<LandscapeVegetationRenderInstances> m_task{};
		};

		void DestroyPhysicsBodies(LandscapeData& component);
		void DestroyChunkPhysicsBodies(LandscapeData& component, LandscapeChunk& chunk);
		bool UpdateGrassResidency(
			const TVector<Math::Transform>& cameraTransforms,
			const TVector<CameraData>& cameras);
		void PublishSceneVersion();
		uint64_t m_shadowCastersRevision = 0u;
		uint64_t m_sceneVersionRevision = 0u;
		uint64_t m_spatialRevision = 0u;
		size_t m_staticSpatialHash = 0u;
		size_t m_stationarySpatialHash = 0u;
		size_t m_dynamicSpatialHash = 0u;
		RHI::RHISpatialSceneVersionPtr m_publishedSceneVersion{};
		RHI::RHIScenePtr m_rhiScene{};
		TMap<size_t, RHI::RenderInstanceHandle> m_renderInstanceHandles{};
		TMap<size_t, uint64_t> m_publishedBuildRevisions{};
		TVector<LandscapeGrassCandidate> m_grassCandidatesScratch{};
		TVector<LandscapeGrassSelection> m_grassSelectionsScratch{};
		TVector<glm::ivec2> m_cameraChunkCoordinatesScratch{};
		TVector<glm::vec3> m_cameraPositionsScratch{};
		TVector<Math::Frustum> m_cameraFrustumsScratch{};
		TVector<GrassTransformBuildRequest> m_grassBuildRequestsScratch{};
	};

	template class ECS::TSystem<LandscapeECS, LandscapeData>;
}
