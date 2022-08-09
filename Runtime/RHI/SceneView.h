#pragma once
#include "Core/Defines.h"
#include "Memory/Memory.h"
#include "Containers/Octree.h"
#include "Engine/Types.h"
#include "RHI/Mesh.h"
#include "RHI/Material.h"
#include "ECS/CameraECS.h"

namespace Sailor::RHI
{
	struct RHIMeshProxy
	{
		size_t m_staticMeshEcs;
		glm::mat4 m_worldMatrix;
		SAILOR_API bool operator==(const RHIMeshProxy& rhs) const { return m_staticMeshEcs == rhs.m_staticMeshEcs; }
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
		TUniquePtr<CameraData> m_camera;
		TVector<RHISceneViewProxy> m_proxies;
		RHIShaderBindingSetPtr m_frameBindings;
		RHICommandListPtr m_debugDrawSecondaryCmdList;
	};

	struct RHISceneView
	{
		SAILOR_API TVector<RHISceneViewSnapshot> GetSnapshots();
		SAILOR_API void PrepareDebugDrawCommandLists(WorldPtr world);

		TOctree<RHIMeshProxy> m_stationaryOctree{ glm::ivec3(0,0,0), 16536 * 2, 4 };
		TOctree<RHISceneViewProxy> m_staticOctree{ glm::ivec3(0,0,0), 16536 * 2, 4 };

		TVector<CameraData> m_cameras;
		TVector<RHI::RHICommandListPtr> m_debugDraw;

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