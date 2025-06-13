#include "ECS/StaticMeshRendererECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "RHI/Material.h"
#include "RHI/Fence.h"
#include "Components/AnimatorComponent.h"

using namespace Sailor;
using namespace Sailor::Tasks;

void StaticMeshRendererECS::BeginPlay()
{
	m_sceneViewProxiesCache = RHI::RHISceneViewPtr::Make();
}

Tasks::ITaskPtr StaticMeshRendererECS::Tick(float deltaTime)
{
	const uint32_t NumComponentsPerTask = 1024;

	SAILOR_PROFILE_FUNCTION();

	//TODO: Resolve New/Delete components
	TVector<Tasks::TaskPtr<TVector<TPair<RHI::RHIMeshProxy, Math::AABB>>>> tasks;
	for (uint32_t i = 0; i < (m_components.Num() / NumComponentsPerTask + 1); i++)
	{
		auto task = Tasks::CreateTask<TVector<TPair<RHI::RHIMeshProxy, Math::AABB>>>("StaticMeshRendererECS:Update Stationary Objects",
			[this, i]()
			{
				TVector<TPair<RHI::RHIMeshProxy, Math::AABB>> temp;

				for (uint32_t j = 0; j < NumComponentsPerTask; j++)
				{
					const uint32_t index = i * NumComponentsPerTask + j;
					if (index >= m_components.Num())
					{
						break;
					}

					auto& data = m_components[index];
					auto ownerGameObject = data.m_owner.StaticCast<GameObject>();
					EMobilityType mobilityType = ownerGameObject->GetMobilityType();

					if (mobilityType == EMobilityType::Stationary && data.m_bIsActive && data.GetModel() && data.GetModel()->IsReady())
					{
						const auto& ownerTransform = ownerGameObject->GetTransformComponent();
						Math::AABB adjustedBounds = data.GetModel()->GetBoundsAABB();

						// Should we update only when transform changed?
						if ((data.m_bIsDirty || data.m_frameLastChange == 0 || ownerTransform.GetFrameLastChange() > data.m_frameLastChange) && adjustedBounds.IsValid())
						{
							RHI::RHIMeshProxy proxy;
							proxy.m_staticMeshEcs = GetComponentIndex(&data);
							proxy.m_worldMatrix = ownerTransform.GetCachedWorldMatrix();

							adjustedBounds.Apply(proxy.m_worldMatrix);

							temp.Emplace(TPair(std::move(proxy), std::move(adjustedBounds)));

							data.m_frameLastChange = ownerTransform.GetFrameLastChange();

							if (data.m_frameLastChange != ownerGameObject->GetFrameLastChange())
							{
								UpdateGameObject(ownerGameObject, GetWorld()->GetCurrentFrame());
							}

							data.m_bIsDirty = false;
						}
					}
				}

				return temp;
			}, EThreadType::Worker);

		if (m_components.Num() < NumComponentsPerTask)
		{
			task->Execute();

			for (auto& t : task->m_result)
			{
				m_sceneViewProxiesCache->m_stationaryOctree.Update(glm::vec4(t.m_second.GetCenter(), 1), t.m_second.GetExtents(), t.m_first);
			}

			break;
		}

		task->Run();
		tasks.Add(task);
	}

	for (auto& task : tasks)
	{
		task->Wait();
		for (auto& t : task->m_result)
		{
			m_sceneViewProxiesCache->m_stationaryOctree.Update(glm::vec4(t.m_second.GetCenter(), 1), t.m_second.GetExtents(), t.m_first);
		}
	}

	auto updateStaticTask = Tasks::CreateTask("StaticMeshRendererECS:Update Static Objects",
		[this]()
		{
			for (auto& data : m_components)
			{
				auto ownerGameObject = data.m_owner.StaticCast<GameObject>();
				EMobilityType mobilityType = ownerGameObject->GetMobilityType();

				if (mobilityType == EMobilityType::Static && data.m_bIsActive && data.GetModel() && data.GetModel()->IsReady())
				{
					auto ownerGameObject = data.m_owner.StaticCast<GameObject>();
					const auto& ownerTransform = ownerGameObject->GetTransformComponent();
					Math::AABB adjustedBounds = data.GetModel()->GetBoundsAABB();

					if ((data.m_bIsDirty || ownerTransform.GetFrameLastChange() > data.m_frameLastChange) && adjustedBounds.IsValid())
					{
						RHI::RHISceneViewProxy proxy;
						proxy.m_staticMeshEcs = GetComponentIndex(&data);
						proxy.m_worldMatrix = ownerTransform.GetCachedWorldMatrix();
						if (auto animator = ownerGameObject->GetComponent<AnimatorComponent>())
						{
							data.m_skeletonOffset = animator->GetSkeletonOffset();
						}
						proxy.m_skeletonOffset = data.m_skeletonOffset;
						proxy.m_meshes = data.GetModel()->GetMeshes();
						proxy.m_worldAabb = data.GetModel()->GetBoundsAABB();
						proxy.m_worldAabb.Apply(proxy.m_worldMatrix);
						proxy.m_bCastShadows = data.ShouldCastShadow();

						proxy.m_overrideMaterials.Clear();
						for (size_t i = 0; i < proxy.m_meshes.Num(); i++)
						{
							size_t materialIndex = (std::min)(i, data.GetMaterials().Num() - 1);
							proxy.m_overrideMaterials.Add(data.GetMaterials()[materialIndex]->GetOrAddRHI(proxy.m_meshes[i]->m_vertexDescription));
						}

						adjustedBounds.Apply(proxy.m_worldMatrix);
						m_sceneViewProxiesCache->m_staticOctree.Update(glm::vec4(adjustedBounds.GetCenter(), 1), adjustedBounds.GetExtents(), proxy);

						data.m_frameLastChange = ownerTransform.GetFrameLastChange();

						if (data.m_frameLastChange == 0 || data.m_frameLastChange != ownerGameObject->GetFrameLastChange())
						{
							UpdateGameObject(ownerGameObject, GetWorld()->GetCurrentFrame());
						}

						data.m_bIsDirty = false;
					}
				}
			}
		}, EThreadType::RHI)->Run();

	updateStaticTask->Wait();

	return nullptr;
}

void StaticMeshRendererECS::CopySceneView(RHI::RHISceneViewPtr& outProxies)
{
	SAILOR_PROFILE_FUNCTION();

	outProxies->m_stationaryOctree = m_sceneViewProxiesCache->m_stationaryOctree;
	outProxies->m_staticOctree = m_sceneViewProxiesCache->m_staticOctree;
}

void StaticMeshRendererECS::EndPlay()
{
	ECS::TSystem<StaticMeshRendererECS, StaticMeshRendererData>::EndPlay();
	m_sceneViewProxiesCache.Clear();
}
