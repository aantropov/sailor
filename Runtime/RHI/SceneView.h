#pragma once
#include "Core/Defines.h"
#include "Memory/Memory.h"
#include "Containers/Octree.h"
#include "Engine/Types.h"
#include "RHI/Mesh.h"
#include "RHI/Material.h"
#include "ECS/CameraECS.h"
#include "Math/Math.h"

namespace Sailor::RHI
{
	enum class EShadowType : uint32_t
	{
		None = 0,
		PCF,
		EVSM
	};

	struct RHIMeshProxy
	{
		size_t m_staticMeshEcs = 0;
		glm::mat4 m_worldMatrix{};
		SAILOR_API bool operator==(const RHIMeshProxy& rhs) const { return m_staticMeshEcs == rhs.m_staticMeshEcs; }
	};

        struct RHILightProxy
        {
                uint32_t m_index = 0;
                float m_distanceToCamera{};
                EShadowType m_shadowType = EShadowType::None;
                float m_evsmBlurScale{ 1.0f };
                glm::mat4 m_lightMatrix{};
                Math::Transform m_cameraTransform{};
                Math::Transform m_lightTransform{};

		SAILOR_API bool operator<(const RHILightProxy& rhs) const { return m_distanceToCamera < rhs.m_distanceToCamera; }
	};

	struct RHISceneViewProxy
	{
		size_t m_staticMeshEcs{};
		glm::mat4 m_worldMatrix;
		Math::AABB m_worldAabb{};
		
		bool m_bCastShadows{};
		size_t m_frame{};

		TVector<RHIMeshPtr> m_meshes;
		TVector<RHIMaterialPtr> m_overrideMaterials;

		SAILOR_API bool operator==(const RHISceneViewProxy& rhs) const { return m_staticMeshEcs == rhs.m_staticMeshEcs; }
		SAILOR_API const TVector<RHIMaterialPtr>& GetMaterials() const;

	};

	struct RHIUpdateShadowMapCommand
	{
		uint32_t m_lighMatrixIndex{};
		EShadowType m_shadowType = EShadowType::None;
		glm::vec2 m_blurRadius{}; // [Umbra, Penumbra]
		RHI::RHIRenderTargetPtr m_shadowMap{};
		glm::mat4 m_lightMatrix{};
		TVector<uint32_t> m_internalCommandsList{};
		TVector<RHISceneViewProxy> m_meshList{};
	};

	struct RHISceneViewSnapshot
	{
		float m_deltaTime = 0.0f;
		uint64_t m_frame = 0ull;
		Math::Transform m_cameraTransform{};
		TUniquePtr<CameraData> m_camera{};
		TVector<RHISceneViewProxy> m_proxies{};

		uint32_t m_totalNumLights = 0;
		TVector<RHIUpdateShadowMapCommand> m_shadowMapsToUpdate{};

		RHIShaderBindingSetPtr m_frameBindings{};
		RHIShaderBindingSetPtr m_rhiLightsData{};

		Tasks::TaskPtr<RHICommandListPtr> m_debugDrawSecondaryCmdList{};
		Tasks::TaskPtr<RHICommandListPtr, void> m_drawImGui{};
	};

	struct RHISceneView
	{
		SAILOR_API TVector<RHISceneViewProxy> TraceScene(const Math::Frustum& frustum, bool bSkipMaterials) const;
		SAILOR_API void PrepareSnapshots();
		SAILOR_API void PrepareDebugDrawCommandLists(WorldPtr world);

		TOctree<RHIMeshProxy> m_stationaryOctree{ glm::ivec3(0,0,0), 16536 * 16, 4 };
		TOctree<RHISceneViewProxy> m_staticOctree{ glm::ivec3(0,0,0), 16536 * 16, 4 };

		uint32_t m_totalNumLights = 0;
		RHI::RHIShaderBindingSetPtr m_rhiLightsData{};

		// For each camera
		TVector<TVector<RHIUpdateShadowMapCommand>> m_shadowMapsToUpdate;

		TVector<CameraData> m_cameras;
		TVector<Math::Transform> m_cameraTransforms;

		Tasks::TaskPtr<RHI::RHICommandListPtr, void> m_drawImGui;
		TVector<Tasks::TaskPtr<RHI::RHICommandListPtr>> m_debugDraw;
		TVector<RHISceneViewSnapshot> m_snapshots;

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
	struct std::hash<Sailor::RHI::RHISceneViewProxy>
	{
		SAILOR_API std::size_t operator()(const Sailor::RHI::RHISceneViewProxy& p) const
		{
			std::hash<size_t> p1;
			return p1(p.m_staticMeshEcs);
		}
	};

	template<>
	struct std::hash<Sailor::RHI::RHIMeshProxy>
	{
		SAILOR_API std::size_t operator()(const Sailor::RHI::RHIMeshProxy& p) const
		{
			std::hash<size_t> p1;
			return p1(p.m_staticMeshEcs);
		}
	};
}