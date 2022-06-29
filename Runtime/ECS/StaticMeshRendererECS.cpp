#include "ECS/StaticMeshRendererECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"

using namespace Sailor;
using namespace Sailor::JobSystem;

JobSystem::ITaskPtr StaticMeshRendererECS::Tick(float deltaTime)
{
	m_sceneViewProxiesCache = RHI::RHISceneViewPtr::Make();

	for (auto& data : m_components)
	{
		if (data.m_bIsActive && data.GetModel()->IsReady())
		{
			const auto& ownerTransform = data.m_owner.StaticCast<GameObject>()->GetTransform();

			RHI::RHISceneViewProxy proxy;
			//proxy.m_overrideMaterials = data.GetMaterials();
			//proxy.m_model = data.GetModel();
			proxy.m_staticMeshEcs = GetComponentIndex(&data);
			proxy.m_worldMatrix = ownerTransform.GetCachedWorldMatrix();

			m_sceneViewProxiesCache->m_octree.Insert(ownerTransform.GetWorldPosition(), data.GetModel()->GetBoundsAABB().GetExtents(), proxy);
		}
	}

	return nullptr;
}

void StaticMeshRendererECS::TakeSceneView(RHI::RHISceneViewPtr& outProxies)
{
	outProxies = std::move(m_sceneViewProxiesCache);
}

