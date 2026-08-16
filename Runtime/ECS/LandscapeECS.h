#pragma once

#include "ECS/ECS.h"
#include "RHI/SceneView.h"

namespace Sailor
{
	enum class ELandscapeVegetationShadowMode : uint8_t
	{
		None = 0u,
		NearOnly,
		All
	};

	struct LandscapeVegetationProfile final
	{
		FileId m_modelFileId{};
		FileId m_materialFileId{};
		ModelPtr m_model{};
		MaterialPtr m_material{};
		TVector<MaterialPtr> m_modelMaterials{};
		bool m_bModelMaterialsRequested = false;
		int32_t m_meshIndex = -1;
		uint32_t m_instancesPerChunk = 0u;
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
		RHI::RHISceneViewProxy m_proxy{};
		glm::ivec3 m_octreeCenter{};
		glm::ivec3 m_octreeExtents{ 1 };
	};

	struct LandscapeChunk final
	{
		RHI::RHISceneViewProxy m_proxy{};
		glm::ivec3 m_octreeCenter{};
		glm::ivec3 m_octreeExtents{ 1 };
		TVector<LandscapeVegetationRenderProxy> m_vegetationProxies{};
	};

	class LandscapeData final : public ECS::TComponent
	{
	public:
		SAILOR_API void SetSettings(uint32_t chunksX, uint32_t chunksZ,
			float chunkSize, uint32_t chunkResolution, float heightScale,
			float noiseScale, uint32_t seed, float textureTiling);
		SAILOR_API void SetMaterial(const MaterialPtr& material);
		SAILOR_API void SetLayerTextures(const TVector<FileId>& textures);
		SAILOR_API void SetImportMaps(const FileId& heightmapTexture,
			const TVector<FileId>& materialMasks);
		SAILOR_API void SetAuthoredStamps(const TVector<float>& sculptStamps,
			const TVector<float>& paintStamps);
		SAILOR_API void SetVegetationProfiles(
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
		MaterialPtr m_material{};
		MaterialPtr m_runtimeMaterial{};
		TVector<FileId> m_layerTextures{};
		FileId m_heightmapTexture{};
		TVector<FileId> m_materialMasks{};
		TVector<float> m_sculptStamps{};
		TVector<float> m_paintStamps{};
		TVector<LandscapeVegetationProfile> m_vegetationProfiles{};
		TVector<LandscapeChunk> m_chunks{};
		TVector<uint32_t> m_physicsBodies{};
		uint64_t m_buildRevision = 0u;

		friend class LandscapeECS;
	};

	class LandscapeECS final : public ECS::TSystem<LandscapeECS, LandscapeData>
	{
	public:
		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual Tasks::ITaskPtr Tick(float deltaTime) override;
		SAILOR_API virtual void EndPlay() override;
		SAILOR_API void AppendSceneView(RHI::RHISceneViewPtr& sceneView) const;
		virtual uint32_t GetOrder() const override { return 990u; }

	protected:
		SAILOR_API virtual void OnComponentUnregistered(size_t index, LandscapeData& component) override;

	private:
		void DestroyPhysicsBodies(LandscapeData& component);
		uint64_t m_shadowCastersRevision = 0u;
	};

	template class ECS::TSystem<LandscapeECS, LandscapeData>;
}
