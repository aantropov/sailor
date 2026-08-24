#pragma once
#include "Core/Defines.h"
#include "Memory/Memory.h"
#include "Containers/Octree.h"
#include "Engine/Types.h"
#include "RHI/Mesh.h"
#include "RHI/Material.h"
#include "RHI/RenderDebugView.h"
#include "RHI/RenderSubmission.h"
#include "RHI/Scene.h"
#include "RHI/Lighting.h"
#include "RHI/GlobalIllumination.h"
#include "ECS/CameraECS.h"
#include "Math/Math.h"
#include "Raytracing/LightingModel.h"
#include "Raytracing/PathTracer.h"

#include <limits>

namespace Sailor::RHI
{
	SAILOR_API float CalculateScreenCoverage(
		const Math::AABB& worldBounds,
		const glm::mat4& viewMatrix,
		const glm::mat4& projectionMatrix);

	enum class EShadowType : uint32_t
	{
		None = 0,
		PCF,
		EVSM
	};

	struct RHIShadowMeshProxy
	{
		RHIMeshPtr m_mesh{};
		glm::mat4 m_worldMatrix{ 1.0f };
		size_t m_renderQueueTag{};
		glm::vec4 m_baseColorFactor{ 1.0f };
		float m_alphaCutoff = 0.5f;
		uint32_t m_baseColorSampler = 0;
		float m_maxCameraDistance = (std::numeric_limits<float>::max)();
		RHIMaterialPtr m_customDepthMaterial{};
		ShaderSetPtr m_customDepthShader{};
#if defined(__APPLE__)
		TSet<uint32_t> m_materialTextureSamplers{};
#endif
	};

	struct RHILodPolicy
	{
		bool m_bEnabled = false;
		uint32_t m_minLod = 0u;
		uint32_t m_maxLod = 2u;
		TVector<float> m_screenCoverageThresholds{ 0.25f, 0.05f };
		TVector<float> m_cameraDistanceThresholds{};
		float m_maxCameraDistance = (std::numeric_limits<float>::infinity)();

		SAILOR_API uint32_t Resolve(float screenCoverage, uint32_t numAvailableLods) const;
		SAILOR_API uint32_t Resolve(
			float screenCoverage,
			float cameraDistance,
			uint32_t numAvailableLods) const;
	};

	struct RHIShadowCasterProxy
	{
		size_t m_staticMeshEcs{};
		EMobilityType m_mobility = EMobilityType::Static;
		Math::AABB m_worldAabb{};
		uint32_t m_skeletonOffset = (std::numeric_limits<uint32_t>::max)();
		size_t m_frame{};
		TVector<RHIShadowMeshProxy> m_meshes{};
		RHILodPolicy m_lodPolicy{};
	};

	using RHIShadowCasterProxyPtr = TSharedPtr<RHIShadowCasterProxy>;

	/**
	 * Immutable topology shared by every instance of one landscape vegetation
	 * profile. Instance transforms are stored once; mesh/material metadata stays
	 * per profile mesh instead of being repeated instance x mesh.
	 */
	struct RHIInstancedMeshGroup
	{
		TVector<glm::mat4> m_instanceTransforms{};
		TVector<int32_t> m_instanceLodBiases{};
		TVector<float> m_instanceCullDistanceScales{};
		TVector<float> m_instanceShadowDistanceScales{};
		TVector<RHIMeshPtr> m_meshes{};
		TVector<glm::mat4> m_meshTransforms{};
		TVector<RHIMaterialPtr> m_materials{};
		TVector<ShaderSetPtr> m_sourceMaterialShaders{};
		TVector<size_t> m_renderQueueTags{};
		TVector<glm::vec4> m_baseColorFactors{};
		TVector<uint32_t> m_baseColorSamplers{};
		TVector<float> m_alphaCutoffs{};
#if defined(__APPLE__)
		TVector<TSet<uint32_t>> m_materialTextureSamplers{};
#endif
		bool m_bCastShadows = false;
		float m_maxShadowDistance = (std::numeric_limits<float>::max)();
	};

	struct RHIMeshProxy
	{
		size_t m_staticMeshEcs = 0;
		glm::mat4 m_worldMatrix{};
		RHIShadowCasterProxyPtr m_shadowCaster{};
		SAILOR_API bool operator==(const RHIMeshProxy& rhs) const { return m_staticMeshEcs == rhs.m_staticMeshEcs; }
	};

	struct RHILightProxy
	{
		uint32_t m_index = 0;
		float m_distanceToCamera{};
		EShadowType m_shadowType = EShadowType::None;
		glm::mat4 m_lightMatrix{};
		Math::Transform m_cameraTransform{};
		Math::Transform m_lightTransform{};

		SAILOR_API bool operator<(const RHILightProxy& rhs) const { return m_distanceToCamera < rhs.m_distanceToCamera; }
	};

	struct RHISceneViewProxy
	{
		size_t m_staticMeshEcs{};
		EMobilityType m_mobility = EMobilityType::Static;
		glm::mat4 m_worldMatrix;
		Math::AABB m_worldAabb{};

		bool m_bCastShadows{};
		uint32_t m_skeletonOffset = (std::numeric_limits<uint32_t>::max)();
		size_t m_frame{};

		TVector<RHIMeshPtr> m_meshes;
		TVector<glm::mat4> m_meshModelMatrices;
		TVector<RHIMaterialPtr> m_overrideMaterials;
		TVector<size_t> m_renderQueueTags;
		TVector<glm::vec4> m_baseColorFactors;
		TVector<uint32_t> m_baseColorSamplers;
		TVector<float> m_alphaCutoffs;
		TVector<RHIInstancedMeshGroup> m_instancedGroups{};
#if defined(__APPLE__)
		TVector<TSet<uint32_t>> m_materialTextureSamplers;
#endif
		RHIShadowCasterProxyPtr m_shadowCaster{};
		RHILodPolicy m_lodPolicy{};

		SAILOR_API bool operator==(const RHISceneViewProxy& rhs) const { return m_staticMeshEcs == rhs.m_staticMeshEcs; }
		SAILOR_API const TVector<RHIMaterialPtr>& GetMaterials() const;

	};

	class RHISceneProxyResource final : public RHIResource
	{
	public:
		RHISceneProxyResource() = default;
		SAILOR_API explicit RHISceneProxyResource(const RHISceneViewProxy& proxy);
		SAILOR_API explicit RHISceneProxyResource(RHISceneViewProxy&& proxy);

		RHISceneViewProxy m_proxy{};
		bool m_bMeshTransformsAreLocal = false;
		size_t m_geometryRevision = 0u;
		size_t m_mainRevision = 0u;
		size_t m_depthRevision = 0u;
		size_t m_shadowRevision = 0u;
	};

	using RHISceneProxyResourcePtr = TRefPtr<RHISceneProxyResource>;

	struct RHIVisibleSceneProxy
	{
		RenderInstanceHandle m_handle{};
		// Non-owning pointers into the immutable record root retained by
		// RHISceneViewSnapshot::m_sceneVersions for the whole submission.
		const RHISceneInstanceRecord* m_record = nullptr;
		const RHISceneProxyResource* m_resource = nullptr;
		float m_screenCoverage = 1.0f;
		float m_cameraDistance = 0.0f;

		SAILOR_API const RHISceneViewProxy* GetSource() const;
		SAILOR_API const glm::mat4& GetWorldMatrix() const;
		SAILOR_API const Math::AABB& GetWorldBounds() const;
		SAILOR_API EMobilityType GetMobility() const;
		SAILOR_API uint32_t GetSkeletonOffset() const;
		SAILOR_API uint32_t GetRenderFlags() const;
		SAILOR_API uint64_t GetContentRevision() const;
		SAILOR_API RHIMeshPtr ResolveMesh(size_t meshIndex) const;
		SAILOR_API RHIMeshPtr ResolveMesh(const RHIMeshPtr& mesh) const;
		SAILOR_API glm::mat4 ResolveMeshWorldMatrix(size_t meshIndex) const;
		SAILOR_API glm::mat4 ResolveInstancedMeshWorldMatrix(
			const RHIInstancedMeshGroup& group,
			size_t instanceIndex,
			size_t meshIndex) const;
		SAILOR_API RHIMeshPtr ResolveInstancedMesh(
			const RHIInstancedMeshGroup& group,
			size_t instanceIndex,
			size_t meshIndex,
			const glm::mat4& viewMatrix,
			const glm::mat4& projectionMatrix) const;
		SAILOR_API bool IsInstancedMeshWithinDistance(
			const RHIInstancedMeshGroup& group,
			size_t instanceIndex,
			size_t meshIndex,
			const glm::vec3& cameraPosition,
			float maxDistance) const;
	};

	struct RHIVisibleShadowCaster
	{
		RenderInstanceHandle m_handle{};
		// The owning RHISceneVersion is retained by the submission snapshot.
		const RHISceneInstanceRecord* m_record = nullptr;
		const RHISceneProxyResource* m_resource = nullptr;
		float m_cameraDistance = 0.0f;

		SAILOR_API const RHIShadowCasterProxy* GetSource() const;
		SAILOR_API const glm::mat4& GetWorldMatrix() const;
		SAILOR_API const Math::AABB& GetWorldBounds() const;
		SAILOR_API EMobilityType GetMobility() const;
		SAILOR_API uint32_t GetSkeletonOffset() const;
		SAILOR_API uint64_t GetProducerKey() const;
		SAILOR_API uint64_t GetContentRevision() const;
		SAILOR_API RHIMeshPtr ResolveMesh(
			const RHIShadowMeshProxy& shadowMesh,
			const glm::mat4& shadowViewProjection) const;
		SAILOR_API RHIMeshPtr ResolveMesh(
			const RHIMeshPtr& mesh,
			const glm::mat4& shadowViewProjection) const;
		SAILOR_API glm::mat4 ResolveMeshWorldMatrix(const RHIShadowMeshProxy& shadowMesh) const;
		SAILOR_API glm::mat4 ResolveInstancedMeshWorldMatrix(
			const RHIInstancedMeshGroup& group,
			size_t instanceIndex,
			size_t meshIndex) const;
		SAILOR_API RHIMeshPtr ResolveInstancedMesh(
			const RHIInstancedMeshGroup& group,
			size_t instanceIndex,
			size_t meshIndex,
			const glm::mat4& shadowViewProjection) const;
		SAILOR_API bool IsInstancedMeshWithinDistance(
			const RHIInstancedMeshGroup& group,
			size_t instanceIndex,
			size_t meshIndex,
			const glm::vec3& cameraPosition,
			float maxDistance) const;
	};

	static_assert(sizeof(RHIVisibleSceneProxy) <= sizeof(void*) * 4u,
		"Visible scene records must remain lightweight immutable references.");
	static_assert(sizeof(RHIVisibleShadowCaster) <= sizeof(void*) * 4u,
		"Visible shadow records must remain lightweight immutable references.");

	/**
	 * Immutable after publication. ECS producers rebuild this adapter only when
	 * their scene data changes; render views retain and share it across submissions.
	 */
	struct RHISpatialSceneVersion
	{
		TSharedPtr<TOctree<RenderInstanceHandle>> m_dynamicOctree{};
		TSharedPtr<TOctree<RenderInstanceHandle>> m_stationaryOctree{};
		TSharedPtr<TOctree<RenderInstanceHandle>> m_staticOctree{};
		uint64_t m_revision = 0ull;
		uint64_t m_shadowCastersRevision = 0ull;
		bool m_bHasCustomDepthShadowCasters = false;
		RHIScenePtr m_scene{};
		RHISceneVersionPtr m_sceneVersion{};
	};

	using RHISpatialSceneVersionPtr = TSharedPtr<RHISpatialSceneVersion>;

	struct RHIPathTracerProxy
	{
		ModelPtr m_model{};
		Math::AABB m_worldBounds{};
		glm::mat4 m_worldMatrix{ 1.0f };
		glm::mat4 m_inverseWorldMatrix{ 1.0f };
		TVector<MaterialPtr> m_materials{};
		uint64_t m_frameLastChange = 0ull;
	};

	struct RHIUpdateShadowMapCommand
	{
		uint32_t m_lighMatrixIndex{};
		EShadowType m_shadowType = EShadowType::None;
		glm::vec2 m_blurRadius{}; // [Umbra, Penumbra]
		glm::ivec4 m_renderArea{}; // [x, y, width, height], zero means the full target
		RHI::RHIRenderTargetPtr m_shadowMap{};
		RHISubmissionCompletionTokenPtr m_payloadCompletionToken{};
		glm::mat4 m_lightMatrix{};
		TVector<uint32_t> m_internalCommandsList{};
		TVector<RHIVisibleShadowCaster> m_meshList{};
	};

	struct RHIBlitShadowMapCommand
	{
		RHI::RHIRenderTargetPtr m_source{};
		RHI::RHIRenderTargetPtr m_destination{};
		glm::ivec4 m_sourceArea{};
		glm::ivec4 m_destinationArea{};
	};

	struct RHISceneViewSnapshot
	{
		SAILOR_API void ResetForReuse();
		SAILOR_API uint64_t GetMobilityRevision(EMobilityType mobility) const;

		template<typename TCallback>
		void ForEachSceneProxy(EMobilityType mobility, TCallback&& callback) const
		{
			if (!m_sceneVersions)
			{
				return;
			}

			for (const auto& sceneVersion : *m_sceneVersions)
			{
				if (!sceneVersion)
				{
					continue;
				}

				const TSharedPtr<TVector<RenderInstanceHandle>>* handles = nullptr;
				switch (mobility)
				{
				case EMobilityType::Static:
					handles = &sceneVersion->m_staticHandles;
					break;
				case EMobilityType::Stationary:
					handles = &sceneVersion->m_stationaryHandles;
					break;
				case EMobilityType::Dynamic:
					handles = &sceneVersion->m_dynamicHandles;
					break;
				}
				if (!handles || !*handles)
				{
					continue;
				}

				for (const auto& handle : **handles)
				{
					const RHISceneInstanceRecord* record = nullptr;
					if (!sceneVersion->Resolve(handle, record) || !record)
					{
						continue;
					}
					const auto* resource = dynamic_cast<const RHISceneProxyResource*>(
						record->m_topology.GetRawPtr());
					if (!resource)
					{
						continue;
					}

					RHIVisibleSceneProxy proxy;
					proxy.m_handle = handle;
					proxy.m_record = record;
					proxy.m_resource = resource;
					callback(proxy);
				}
			}
		}

		template<typename TCallback>
		void ForEachShadowCaster(EMobilityType mobility, TCallback&& callback) const
		{
			ForEachSceneProxy(mobility, [&](const RHIVisibleSceneProxy& sceneProxy)
			{
				if (!sceneProxy.m_record ||
					(sceneProxy.m_record->m_renderFlags & 1u) == 0u ||
					!sceneProxy.m_resource ||
					!sceneProxy.m_resource->m_proxy.m_shadowCaster)
				{
					return;
				}

				RHIVisibleShadowCaster caster;
				caster.m_handle = sceneProxy.m_handle;
				caster.m_record = sceneProxy.m_record;
				caster.m_resource = sceneProxy.m_resource;
				callback(caster);
			});
		}

		RHIRenderSubmissionContextPtr m_submissionContext{};
		TSharedPtr<TVector<RHISceneVersionPtr>> m_sceneVersions{};
		uint64_t m_sceneRevision = 0ull;
		ESceneViewRenderMode m_renderMode = ESceneViewRenderMode::Lit;
		float m_deltaTime = 0.0f;
		uint64_t m_frame = 0ull;
		uint32_t m_cameraIndex = 0u;
		Math::Transform m_cameraTransform{};
		TUniquePtr<CameraData> m_camera{};
		TVector<RHIVisibleSceneProxy> m_proxies{};
		TVector<RHIPathTracerProxy> m_pathTracerProxies{};
		TVector<Sailor::Raytracing::PathTracer::TLASInstance> m_pathTracerTLASInstances{};
		TVector<MaterialPtr> m_pathTracerMaterials{};
		TVector<Sailor::Raytracing::LightProxy> m_pathTracerLights{};

		uint32_t m_totalNumLights = 0;
		TVector<RHIUpdateShadowMapCommand> m_shadowMapsToUpdate{};
		TVector<RHIBlitShadowMapCommand> m_shadowMapsToBlit{};
		TVector<uint32_t> m_shadowIndices{};
		TVector<uint32_t> m_shadowAtlasTiles{};

		RHIShaderBindingSetPtr m_frameBindings{};
		RHIShaderBindingSetPtr m_rhiLightsData{};
		RHIShaderBindingSetPtr m_rhiLightCullingData{};
		TSharedPtr<TVector<RHILightShaderData>> m_cpuLightsData{};
		TVector<glm::mat4> m_shadowMatrices{};
		uint64_t m_lightingRevision = 0ull;
		RHIShaderBindingSetPtr m_boneMatrices{};
		TSharedPtr<TVector<glm::mat4>> m_cpuBoneMatrices{};
		uint64_t m_animationRevision = 0ull;
		EGlobalIlluminationMode m_globalIlluminationMode =
			EGlobalIlluminationMode::RealtimeAndBaked;
		bool m_bGlobalIlluminationEnabled = true;
		RHIGlobalIlluminationSnapshotPtr m_globalIllumination{};

		Tasks::TaskPtr<RHICommandListPtr> m_debugDrawSecondaryCmdList{};
		Tasks::TaskPtr<RHICommandListPtr, void> m_drawImGui{};
	};

	struct RHISceneView
	{
		SAILOR_API TVector<RHIVisibleSceneProxy> TraceScene(const Math::Frustum& frustum, bool bSkipMaterials) const;
		SAILOR_API void TraceScene(
			const Math::Frustum& frustum,
			TVector<RHIVisibleSceneProxy>& outVisibleProxies,
			bool bSkipMaterials) const;
		SAILOR_API TVector<RHIVisibleShadowCaster> TraceShadowCasters(
			const Math::Frustum& frustum,
			const glm::vec3& lodReferencePosition) const;
		SAILOR_API void TraceShadowCasters(
			const Math::Frustum& frustum,
			const glm::vec3& lodReferencePosition,
			TVector<RHIVisibleShadowCaster>& outVisibleCasters) const;
		SAILOR_API void PrepareSnapshots();
		SAILOR_API void PrepareDebugDrawCommandLists(
			WorldPtr world,
			const glm::ivec2& renderExtent);
		SAILOR_API void SetSubmissionContext(RHIRenderSubmissionContextPtr submissionContext);
		SAILOR_API RHISubmissionCompletionTokenPtr GetOrCreateSubmissionCompletionToken();
		SAILOR_API bool IsCurrentSubmissionCompletionToken(
			const RHISubmissionCompletionTokenPtr& token) const;
		SAILOR_API void CompleteSubmissionResources(bool bSucceeded);
		SAILOR_API void AddSceneVersion(RHISpatialSceneVersionPtr sceneVersion);
		SAILOR_API TSharedPtr<TVector<RHISceneVersionPtr>> GetRetainedSceneVersions();

		TVector<RHISpatialSceneVersionPtr> m_sceneVersions{};
		TVector<RHISceneVersionPtr> m_virtualSceneVersions{};
		TSharedPtr<TVector<RHISceneVersionPtr>> m_retainedSceneVersions{};
		uint64_t m_sceneRevision = 0ull;
		ESceneViewRenderMode m_renderMode = ESceneViewRenderMode::Lit;

		uint32_t m_totalNumLights = 0;
		RHI::RHIShaderBindingSetPtr m_rhiLightsData{};
		TVector<RHI::RHIShaderBindingSetPtr> m_rhiLightsDataPerCamera{};
		TSharedPtr<TVector<RHILightShaderData>> m_cpuLightsData{};
		uint64_t m_lightingRevision = 0ull;
		RHI::RHIShaderBindingSetPtr m_boneMatrices{};
		TSharedPtr<TVector<glm::mat4>> m_cpuBoneMatrices{};
		uint64_t m_animationRevision = 0ull;
		EGlobalIlluminationMode m_globalIlluminationMode =
			EGlobalIlluminationMode::RealtimeAndBaked;
		bool m_bGlobalIlluminationEnabled = true;
		RHIGlobalIlluminationSnapshotPtr m_globalIllumination{};

		// For each camera
		TVector<TVector<RHIUpdateShadowMapCommand>> m_shadowMapsToUpdate;
		TVector<TVector<RHIBlitShadowMapCommand>> m_shadowMapsToBlit;
		TVector<TVector<uint32_t>> m_shadowIndices;
		TVector<TVector<uint32_t>> m_shadowAtlasTiles;
		TVector<TVector<glm::mat4>> m_shadowMatrices;

		TVector<CameraData> m_cameras;
		TVector<Math::Transform> m_cameraTransforms;
		TVector<RHIPathTracerProxy> m_pathTracerProxies;
		TVector<Sailor::Raytracing::PathTracer::TLASInstance> m_pathTracerTLASInstances;
		TVector<MaterialPtr> m_pathTracerMaterials;
		TVector<Sailor::Raytracing::LightProxy> m_pathTracerLights;

		Tasks::TaskPtr<RHI::RHICommandListPtr, void> m_drawImGui;
		TVector<Tasks::TaskPtr<RHI::RHICommandListPtr>> m_debugDraw;
		TVector<RHISceneViewSnapshot> m_snapshots;
		RHIRenderSubmissionContextPtr m_submissionContext{};
		RHISubmissionCompletionTokenPtr m_submissionCompletionToken{};
		uint64_t m_shadowCastersRevision = 0;
		bool m_bHasCustomDepthShadowCasters = false;

		WorldPtr m_world{};
		float m_deltaTime{};
		float m_currentTime{};

	public:
		void Clear();
	};

	using RHISceneViewPtr = TSharedPtr<RHISceneView>;
};

namespace std
{
	template<>
	struct hash<Sailor::RHI::RHISceneViewProxy>
	{
		SAILOR_API std::size_t operator()(const Sailor::RHI::RHISceneViewProxy& p) const
		{
			std::hash<size_t> p1;
			return p1(p.m_staticMeshEcs);
		}
	};

	template<>
	struct hash<Sailor::RHI::RHIMeshProxy>
	{
		SAILOR_API std::size_t operator()(const Sailor::RHI::RHIMeshProxy& p) const
		{
			std::hash<size_t> p1;
			return p1(p.m_staticMeshEcs);
		}
	};
}
