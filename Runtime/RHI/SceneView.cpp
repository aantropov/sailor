#include "SceneView.h"
#include "SceneViewProxy.h"
#include "ECS/CameraECS.h"
#include "ECS/TransformECS.h"
#include "ECS/StaticMeshRendererECS.h"
#include "Engine/GameObject.h"
#include "Math/Transform.h"

using namespace Sailor;
using namespace Sailor::RHI;

void RHISceneViewSnapshot::Snapshot(const CameraData& camera, const RHISceneViewPtr& sceneView)
{
	assert(sceneView);

	Math::Frustum frustum;
	auto pCameraOwner = camera.GetOwner();
	auto cameraOwner = pCameraOwner.StaticCast<GameObject>();
	auto transform = cameraOwner->GetTransform().GetTransform();

	frustum.ExtractFrustumPlanes(transform, camera.GetAspect(), camera.GetFov(), camera.GetZNear(), camera.GetZFar());
	sceneView->m_octree.Trace(frustum, m_meshes);
}

const TVector<RHIMaterialPtr>& RHISceneViewProxy::GetMaterials() const
{
	// TODO: Create default materials inside model
	return m_overrideMaterials.Num() > 0 ? m_overrideMaterials : m_overrideMaterials;
}