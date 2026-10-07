#pragma once

#include "ECS/ECS.h"
#include "Landscape/LandscapeData.h"
#include "Landscape/LandscapeStreaming.h"

namespace Sailor
{
	class CameraData;

	class LandscapeECS final : public ECS::TSystem<LandscapeECS, LandscapeData>
	{
	public:
		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void Tick(float deltaTime) override;
		SAILOR_API virtual void EndPlay() override;
		SAILOR_API void MarkDirty(GameObjectPtr owner);
		SAILOR_API void AppendSceneView(RHI::RHISceneViewPtr& sceneView);
		SAILOR_API bool CollectBakeGeometrySnapshots(
			TVector<LandscapeBakeGeometrySnapshot>& outSnapshots,
			std::string& outDiagnostic) const;
		SAILOR_API uint64_t GetGlobalIlluminationContributorRevision()
			const noexcept;
		SAILOR_API uint64_t GetGlobalIlluminationGeometryRevision() const noexcept;
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
		void DestroyPhysicsBody(LandscapeData& component, uint32_t& bodyId);
		void DestroyChunkPhysicsBodies(LandscapeData& component, LandscapeChunk& chunk);
		void CreateChunkPhysicsBodies(LandscapeData& component, LandscapeChunk& chunk,
			const TVector<glm::vec3>& vertices, const TVector<uint32_t>& indices, const Math::Transform& transform);
		void UpdatePhysicsTransform(LandscapeData& component, const Math::Transform& transform);
		void RebuildVegetationPhysics(LandscapeData& component, LandscapeChunk& chunk, const Math::Transform& transform);
		void UpdateTerrainRenderProxy(size_t componentIndex, size_t chunkIndex, RHI::RHIMeshPtr mesh);
		void UpdateVegetationResources(LandscapeData& component);
		void UpdateChunkVegetation(size_t componentIndex, size_t chunkIndex,
			const TVector<LandscapeVegetationInstance>& placements, const TVector<uint32_t>& profiles);
		bool UpdateVegetationSources(size_t componentIndex, uint64_t previousTerrainRevision);
		bool UpdateVegetationRenderProxies(size_t componentIndex);
		bool UpdatePendingVegetation(size_t componentIndex);
		bool UpdateGrassResidency(
			const TVector<Math::Transform>& cameraTransforms,
			const TVector<CameraData>& cameras);
		void PublishSceneVersion();
		bool m_bHasPendingSceneChanges = false;
		uint64_t m_shadowCastersRevision = 0u;
		uint64_t m_sceneVersionRevision = 0u;
		uint64_t m_spatialRevision = 0u;
		size_t m_staticSpatialHash = 0u;
		size_t m_stationarySpatialHash = 0u;
		size_t m_dynamicSpatialHash = 0u;
		RHI::RHISpatialSceneVersionPtr m_publishedSceneVersion{};
		RHI::RHIScenePtr m_rhiScene{};
		TMap<size_t, RHI::RenderInstanceHandle> m_renderInstanceHandles{};
		TVector<LandscapeGrassCandidate> m_grassCandidatesScratch{};
		TVector<LandscapeGrassSelection> m_grassSelectionsScratch{};
		TVector<glm::ivec2> m_cameraChunkCoordinatesScratch{};
		TVector<glm::vec3> m_cameraPositionsScratch{};
		TVector<Math::Frustum> m_cameraFrustumsScratch{};
		TVector<GrassTransformBuildRequest> m_grassBuildRequestsScratch{};
	};

	template class ECS::TSystem<LandscapeECS, LandscapeData>;
}
