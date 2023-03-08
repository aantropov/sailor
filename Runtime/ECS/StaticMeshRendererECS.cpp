#include "ECS/StaticMeshRendererECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "RHI/Material.h"
#include "RHI/Fence.h"

using namespace Sailor;
using namespace Sailor::Tasks;

void StaticMeshRendererECS::BeginPlay()
{
	m_sceneViewProxiesCache = RHI::RHISceneViewPtr::Make();
}

Tasks::ITaskPtr StaticMeshRendererECS::Tick(float deltaTime)
{
	SAILOR_PROFILE_FUNCTION();

	//TODO: Resolve New/Delete components

	auto updateStationaryTask = Tasks::Scheduler::CreateTask("StaticMeshRendererECS:Update Stationary Objects",
		[this]()
	{
		for (auto& data : m_components)
		{
			EMobilityType mobilityType = data.GetOwner().StaticCast<GameObject>()->GetMobilityType();

			if (mobilityType == EMobilityType::Stationary && data.m_bIsActive && data.GetModel() && data.GetModel()->IsReady())
			{
				const auto& ownerTransform = data.m_owner.StaticCast<GameObject>()->GetTransformComponent();
				if (ownerTransform.GetLastFrameChanged() > data.m_lastChanges)
				{
					RHI::RHIMeshProxy proxy;
					proxy.m_staticMeshEcs = GetComponentIndex(&data);
					proxy.m_worldMatrix = ownerTransform.GetCachedWorldMatrix();

					Math::AABB adjustedBounds = data.GetModel()->GetBoundsAABB();
					adjustedBounds.Apply(proxy.m_worldMatrix);
					m_sceneViewProxiesCache->m_stationaryOctree.Update(glm::vec4(adjustedBounds.GetCenter(), 1), adjustedBounds.GetExtents(), proxy);

					data.m_lastChanges = ownerTransform.GetLastFrameChanged();
				}
			}
		}
	}, EThreadType::RHI)->Run();

	auto updateStaticTask = Tasks::Scheduler::CreateTask("StaticMeshRendererECS:Update Static Objects",
		[this]()
	{
		for (auto& data : m_components)
		{
			EMobilityType mobilityType = data.GetOwner().StaticCast<GameObject>()->GetMobilityType();

			if (mobilityType == EMobilityType::Static && data.m_bIsActive && data.GetModel() && data.GetModel()->IsReady())
			{
				const auto& ownerTransform = data.m_owner.StaticCast<GameObject>()->GetTransformComponent();
				if (ownerTransform.GetLastFrameChanged() > data.m_lastChanges)
				{
					RHI::RHISceneViewProxy proxy;
					proxy.m_staticMeshEcs = GetComponentIndex(&data);
					proxy.m_worldMatrix = ownerTransform.GetCachedWorldMatrix();
					proxy.m_meshes = data.GetModel()->GetMeshes();

					proxy.m_overrideMaterials.Clear();
					for (size_t i = 0; i < proxy.m_meshes.Num(); i++)
					{
						size_t materialIndex = (std::min)(i, data.GetMaterials().Num() - 1);
						proxy.m_overrideMaterials.Add(data.GetMaterials()[materialIndex]->GetOrAddRHI(proxy.m_meshes[i]->m_vertexDescription));
					}

					Math::AABB adjustedBounds = data.GetModel()->GetBoundsAABB();
					adjustedBounds.Apply(proxy.m_worldMatrix);
					m_sceneViewProxiesCache->m_staticOctree.Update(glm::vec4(adjustedBounds.GetCenter(), 1), adjustedBounds.GetExtents(), proxy);
					
					data.m_lastChanges = ownerTransform.GetLastFrameChanged();
				}
			}
		}
	}, EThreadType::RHI)->Run();

	updateStaticTask->Wait();
	updateStationaryTask->Wait();

	return nullptr;
}

void StaticMeshRendererECS::CopySceneView(RHI::RHISceneViewPtr& outProxies)
{
	outProxies->m_stationaryOctree = m_sceneViewProxiesCache->m_stationaryOctree;
	outProxies->m_staticOctree = m_sceneViewProxiesCache->m_staticOctree;
}

void StaticMeshRendererECS::EndPlay()
{
	ECS::TSystem<StaticMeshRendererECS, StaticMeshRendererData>::EndPlay();
	m_sceneViewProxiesCache.Clear();
}
