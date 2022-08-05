#include "SceneView.h"
#include "ECS/CameraECS.h"
#include "ECS/TransformECS.h"
#include "ECS/StaticMeshRendererECS.h"
#include "Engine/GameObject.h"
#include "Math/Transform.h"
#include "Engine/World.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"

using namespace Sailor;
using namespace Sailor::RHI;

TVector<RHISceneViewSnapshot> RHISceneView::GetSnapshots()
{
	SAILOR_PROFILE_FUNCTION();

	TVector<RHISceneViewSnapshot> snapshots;
	for (auto& camera : m_cameras)
	{
		RHISceneViewSnapshot res;

		Math::Frustum frustum;

		auto pCameraOwner = camera.GetOwner().StaticCast<GameObject>();
		auto transform = pCameraOwner->GetTransformComponent().GetTransform();

		frustum.ExtractFrustumPlanes(transform, camera.GetAspect(), camera.GetFov(), camera.GetZNear(), camera.GetZFar());

		res.m_camera = TUniquePtr<CameraData>::Make();
		*res.m_camera = camera;

		TVector<RHIMeshProxy> meshProxies;
		m_octree.Trace(frustum, meshProxies);

		res.m_proxies.Reserve(meshProxies.Num());
		for (auto& meshProxy : meshProxies)
		{
			auto& ecsData = m_world->GetECS<StaticMeshRendererECS>()->GetComponentData(meshProxy.m_staticMeshEcs);
			
			RHISceneViewProxy viewProxy;
			viewProxy.m_staticMeshEcs = meshProxy.m_staticMeshEcs;
			viewProxy.m_worldMatrix = meshProxy.m_worldMatrix;
			viewProxy.m_meshes = ecsData.GetModel()->GetMeshes();
			viewProxy.m_overrideMaterials.Clear();
			for (size_t i = 0; i < viewProxy.m_meshes.Num(); i++)
			{
				size_t materialIndex = (std::min)(i, ecsData.GetMaterials().Num() - 1);
				viewProxy.m_overrideMaterials.Add(ecsData.GetMaterials()[materialIndex]->GetOrAddRHI(viewProxy.m_meshes[i]->m_vertexDescription));
			}

			res.m_proxies.Emplace(std::move(viewProxy));
		}

		snapshots.Emplace(std::move(res));
	}

	return snapshots;
}

const TVector<RHIMaterialPtr>& RHISceneViewProxy::GetMaterials() const
{
	// TODO: Create default materials inside model
	return m_overrideMaterials.Num() > 0 ? m_overrideMaterials : m_overrideMaterials;
}