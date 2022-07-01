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
	struct RHISceneViewProxy
	{
		TVector<RHIMeshPtr> m_meshes;
		TVector<RHIMaterialPtr> m_overrideMaterials;
		glm::mat4 m_worldMatrix;

		SAILOR_API bool operator==(const RHISceneViewProxy& rhs) const { return m_staticMeshEcs == rhs.m_staticMeshEcs; }
		SAILOR_API const TVector<RHIMaterialPtr>& GetMaterials() const;

		size_t m_staticMeshEcs;
	};

	struct RHISceneViewSnapshot
	{
		TUniquePtr<CameraData> m_camera;
		TVector<RHISceneViewProxy> m_meshes;
	};

	struct RHISceneView
	{
		SAILOR_API TVector<RHISceneViewSnapshot> GetSnapshots();

		TOctree<RHISceneViewProxy> m_octree;
		TVector<CameraData> m_cameras;
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
}