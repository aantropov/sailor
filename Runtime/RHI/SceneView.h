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
	struct RHIMeshProxy
	{
		size_t m_staticMeshEcs = 0;
		glm::mat4 m_worldMatrix{};
		SAILOR_API bool operator==(const RHIMeshProxy& rhs) const { return m_staticMeshEcs == rhs.m_staticMeshEcs; }
	};

	struct RHILightProxy
	{
		glm::mat4 m_lightMatrix{};
		RHITexturePtr m_shadowMap = nullptr;
		uint32_t m_index = 0;
		size_t m_lastShadowMapUpdate = 0;
		float m_distanceToCamera{};

		SAILOR_API bool operator<(const RHILightProxy& rhs) const { return m_distanceToCamera < rhs.m_distanceToCamera; }
	};

	struct RHISceneViewProxy
	{
		size_t m_staticMeshEcs;
		glm::mat4 m_worldMatrix;

		TVector<RHIMeshPtr> m_meshes;
		TVector<RHIMaterialPtr> m_overrideMaterials;

		SAILOR_API bool operator==(const RHISceneViewProxy& rhs) const { return m_staticMeshEcs == rhs.m_staticMeshEcs; }
		SAILOR_API const TVector<RHIMaterialPtr>& GetMaterials() const;

	};

	struct RHISceneViewSnapshot
	{
		float m_deltaTime = 0.0f;
		Math::Transform m_cameraTransform{};
		TUniquePtr<CameraData> m_camera{};
		TVector<RHISceneViewProxy> m_proxies{};

		uint32_t m_totalNumLights = 0;
		TVector<RHILightProxy> m_sortedPointLights{};
		TVector<RHILightProxy> m_sortedSpotLights{};
		TVector<RHILightProxy> m_directionalLights{};

		RHIShaderBindingSetPtr m_frameBindings{};
		RHI::RHIShaderBindingSetPtr m_rhiLightsData{};

		Tasks::TaskPtr<RHICommandListPtr> m_debugDrawSecondaryCmdList{};
		Tasks::TaskPtr<RHI::RHICommandListPtr, void> m_drawImGui{};
	};

	struct RHISceneView
	{
		SAILOR_API void PrepareSnapshots();
		SAILOR_API void PrepareDebugDrawCommandLists(WorldPtr world);

		TOctree<RHIMeshProxy> m_stationaryOctree{ glm::ivec3(0,0,0), 16536 * 2, 4 };
		TOctree<RHISceneViewProxy> m_staticOctree{ glm::ivec3(0,0,0), 16536 * 2, 4 };

		uint32_t m_totalNumLights = 0;
		RHI::RHIShaderBindingSetPtr m_rhiLightsData{};
		TVector<RHILightProxy> m_directionalLights{};
		// For each camera
		TVector<TVector<RHILightProxy>> m_sortedPointLights{};
		TVector<TVector<RHILightProxy>> m_sortedSpotLights{};

		TVector<CameraData> m_cameras;
		TVector<Math::Transform> m_cameraTransforms;

		Tasks::TaskPtr<RHI::RHICommandListPtr, void> m_drawImGui;
		TVector<Tasks::TaskPtr<RHI::RHICommandListPtr>> m_debugDraw;
		TVector<RHISceneViewSnapshot> m_snapshots;

		WorldPtr m_world;
		float m_deltaTime;
		float m_currentTime;
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