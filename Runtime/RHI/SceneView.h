#pragma once
#include "Memory/RefPtr.hpp"
#include "Renderer.h"
#include "Types.h"
#include "Containers/Octree.h"
#include "Math/Math.h"
#include "Math/Bounds.h"
#include "ECS/TransformECS.h"
#include "AssetRegistry/Model/ModelImporter.h"

namespace Sailor::RHI
{
	struct RHISceneViewProxy
	{
	public:

		ModelPtr m_model;
		TVector<MaterialPtr> m_materials;
		Transform m_transform;

		bool operator==(const RHISceneViewProxy& rhs) const { return m_transformEcs == rhs.m_transformEcs && m_staticMeshEcs == rhs.m_staticMeshEcs; }

		size_t m_transformEcs;
		size_t m_staticMeshEcs;
	};

	class RHISceneView
	{
	public:

		TUniquePtr<class CameraData> m_camera;
		TVector<RHISceneViewProxy>& m_meshes;
	};

	class RHISceneViewFamily
	{
	public:

		void Snapshot(WorldPtr world);
		RHISceneView CreateSceneView(class CameraData& cameraData);

	protected:

		TOctree<RHISceneViewProxy> m_octree{};
	};
};
