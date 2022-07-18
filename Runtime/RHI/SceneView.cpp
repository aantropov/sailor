#include "SceneView.h"
#include "ECS/CameraECS.h"
#include "ECS/TransformECS.h"
#include "ECS/StaticMeshRendererECS.h"
#include "Engine/GameObject.h"
#include "Math/Transform.h"

using namespace Sailor;
using namespace Sailor::RHI;

TVector<RHISceneViewSnapshot> RHISceneView::GetSnapshots()
{
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

		m_octree.Trace(frustum, res.m_proxies);

		snapshots.Emplace(std::move(res));
	}

	return snapshots;
}

const TVector<RHIMaterialPtr>& RHISceneViewProxy::GetMaterials() const
{
	// TODO: Create default materials inside model
	return m_overrideMaterials.Num() > 0 ? m_overrideMaterials : m_overrideMaterials;
}