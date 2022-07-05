#include "ECS/StaticMeshRendererECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "AssetRegistry/Model/ModelImporter.h"

using namespace Sailor;
using namespace Sailor::Tasks;

void StaticMeshRendererECS::BeginPlay()
{
	m_sceneViewProxiesCache = RHI::RHISceneViewPtr::Make();
}

Tasks::ITaskPtr StaticMeshRendererECS::Tick(float deltaTime)
{
	m_sceneViewProxiesCache->m_octree.Clear();

	for (auto& data : m_components)
	{
		if (data.m_bIsActive && data.GetModel() && data.GetModel()->IsReady())
		{
			const auto& ownerTransform = data.m_owner.StaticCast<GameObject>()->GetTransformComponent();

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

void StaticMeshRendererECS::CopySceneView(RHI::RHISceneViewPtr& outProxies)
{
	outProxies->m_octree = m_sceneViewProxiesCache->m_octree;
}

