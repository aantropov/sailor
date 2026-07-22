#include "ECS/StaticMeshRendererECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "RHI/Material.h"
#include "RHI/Fence.h"
#include "Components/AnimatorComponent.h"

using namespace Sailor;
using namespace Sailor::Tasks;

void StaticMeshRendererECS::BeginPlay()
{
	m_sceneViewProxiesCache = RHI::RHISceneViewPtr::Make();
}

void StaticMeshRendererECS::OnComponentUnregistered(size_t index, StaticMeshRendererData& component)
{
	if (!m_sceneViewProxiesCache)
	{
		return;
	}

	RHI::RHIMeshProxy stationaryProxy{};
	stationaryProxy.m_staticMeshEcs = index;
	m_sceneViewProxiesCache->m_stationaryOctree.Remove(stationaryProxy);

	RHI::RHISceneViewProxy staticProxy{};
	staticProxy.m_staticMeshEcs = index;
	m_sceneViewProxiesCache->m_staticOctree.Remove(staticProxy);
}

Tasks::ITaskPtr StaticMeshRendererECS::Tick(float deltaTime)
{
	const uint32_t NumComponentsPerTask = 1024;

	SAILOR_PROFILE_FUNCTION();

	if (!m_sceneViewProxiesCache)
	{
		return nullptr;
	}

	// Remove proxies which no longer belong in their previous mobility tree before
	// producing updates. Marking moved entries dirty lets the opposite tree receive
	// the proxy during this same tick.
	for (size_t index = 0; index < m_components.Num(); index++)
	{
		auto& data = m_components[index];
		GameObjectPtr ownerGameObject = data.m_owner.StaticCast<GameObject>();
		const bool bHasRenderableModel = data.GetModel() && data.GetModel()->IsReady() && data.GetModel()->GetBoundsAABB().IsValid();
		const bool bShouldBeStationary = data.m_bIsActive && ownerGameObject && bHasRenderableModel &&
			ownerGameObject->GetMobilityType() == EMobilityType::Stationary;

		if (!bShouldBeStationary)
		{
			RHI::RHIMeshProxy proxy{};
			proxy.m_staticMeshEcs = index;
			if (m_sceneViewProxiesCache->m_stationaryOctree.Remove(proxy))
			{
				data.m_bIsDirty = true;
			}
		}
	}

	auto cleanupStaticTask = Tasks::CreateTask("StaticMeshRendererECS:Remove Stale Static Objects",
		[this]()
		{
			for (size_t index = 0; index < m_components.Num(); index++)
			{
				auto& data = m_components[index];
				GameObjectPtr ownerGameObject = data.m_owner.StaticCast<GameObject>();
				const bool bHasRenderableModel = data.GetModel() && data.GetModel()->IsReady() && data.GetModel()->GetBoundsAABB().IsValid();
				bool bHasRenderableMaterials = !data.GetMaterials().IsEmpty();
				for (const auto& material : data.GetMaterials())
				{
					bHasRenderableMaterials &= material && material->IsReady();
				}

				const bool bShouldBeStatic = data.m_bIsActive && ownerGameObject && bHasRenderableModel && bHasRenderableMaterials &&
					ownerGameObject->GetMobilityType() == EMobilityType::Static;

				if (!bShouldBeStatic)
				{
					RHI::RHISceneViewProxy proxy{};
					proxy.m_staticMeshEcs = index;
					if (m_sceneViewProxiesCache->m_staticOctree.Remove(proxy))
					{
						data.m_bIsDirty = true;
					}
				}
			}
		}, EThreadType::RHI)->Run();

	cleanupStaticTask->Wait();

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
					if (!data.m_bIsActive)
					{
						continue;
					}

					GameObjectPtr ownerGameObject = data.m_owner.StaticCast<GameObject>();
					if (!ownerGameObject)
					{
						continue;
					}

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
							if (auto animator = ownerGameObject->GetComponent<AnimatorComponent>())
							{
								data.m_skeletonOffset = animator->GetSkeletonOffset();
							}
							else
							{
								data.m_skeletonOffset = StaticMeshRendererData::InvalidSkeletonOffset;
							}

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
			for (size_t index = 0; index < m_components.Num(); index++)
			{
				auto& data = m_components[index];
				if (!data.m_bIsActive)
				{
					continue;
				}

				GameObjectPtr ownerGameObject = data.m_owner.StaticCast<GameObject>();
				if (!ownerGameObject)
				{
					continue;
				}

				EMobilityType mobilityType = ownerGameObject->GetMobilityType();

				if (mobilityType == EMobilityType::Static && data.m_bIsActive && data.GetModel() && data.GetModel()->IsReady())
				{
					const auto& ownerTransform = ownerGameObject->GetTransformComponent();
					Math::AABB adjustedBounds = data.GetModel()->GetBoundsAABB();
					bool bMaterialsReady = !data.GetMaterials().IsEmpty();
					for (const auto& material : data.GetMaterials())
					{
						bMaterialsReady &= material && material->IsReady();
					}

					if ((data.m_bIsDirty || ownerTransform.GetFrameLastChange() > data.m_frameLastChange) &&
						adjustedBounds.IsValid() && bMaterialsReady)
					{
						RHI::RHISceneViewProxy proxy;
						proxy.m_staticMeshEcs = index;
						proxy.m_worldMatrix = ownerTransform.GetCachedWorldMatrix();
						if (auto animator = ownerGameObject->GetComponent<AnimatorComponent>())
						{
							data.m_skeletonOffset = animator->GetSkeletonOffset();
						}
						else
						{
							data.m_skeletonOffset = StaticMeshRendererData::InvalidSkeletonOffset;
						}
						proxy.m_skeletonOffset = data.m_skeletonOffset;
						proxy.m_meshes = data.GetModel()->GetMeshes();
						proxy.m_worldAabb = data.GetModel()->GetBoundsAABB();
						proxy.m_worldAabb.Apply(proxy.m_worldMatrix);
						proxy.m_bCastShadows = data.ShouldCastShadow();

						proxy.m_overrideMaterials.Clear();
#if defined(__APPLE__)
						proxy.m_materialTextureSamplers.Clear();
						proxy.m_materialTextureSamplers.Reserve(proxy.m_meshes.Num());
						auto textureImporter = App::GetSubmodule<TextureImporter>();
#endif
						for (size_t i = 0; i < proxy.m_meshes.Num(); i++)
						{
							size_t materialIndex = (std::min)(i, data.GetMaterials().Num() - 1);
							auto& material = data.GetMaterials()[materialIndex];
							proxy.m_overrideMaterials.Add(material->GetOrAddRHI(proxy.m_meshes[i]->m_vertexDescription));
#if defined(__APPLE__)
							TSet<uint32_t> requestedTextures;
							requestedTextures.Insert(0u);

							if (textureImporter)
							{
								for (const auto& sampler : material->GetSamplers())
								{
									const uint32_t textureIndex = sampler.m_second ? (uint32_t)textureImporter->GetTextureIndex(sampler.m_second->GetFileId()) : 0u;
									requestedTextures.Insert(textureIndex);
								}
							}

							proxy.m_materialTextureSamplers.Add(std::move(requestedTextures));
#endif
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
