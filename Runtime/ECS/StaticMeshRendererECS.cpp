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

namespace
{
	bool HasRenderableSelection(const StaticMeshRendererData& data)
	{
		const ModelPtr& model = data.GetModel();
		return model && model->IsReady() &&
			model->GetBoundsAABB(data.GetMeshIndex()).IsValid();
	}

	bool CollectComponentRenderData(
		const StaticMeshRendererData& data,
		const glm::mat4& ownerWorldMatrix,
		TVector<RHI::RHIMeshPtr>& outMeshes,
		TVector<glm::mat4>& outWorldMatrices,
		Math::AABB& outWorldBounds)
	{
		if (!HasRenderableSelection(data))
		{
			return false;
		}

		TVector<glm::mat4> modelMatrices;
		Math::AABB modelBounds;
		if (!data.GetModel()->CollectRenderData(
				data.GetMeshIndex(),
				outMeshes,
				modelMatrices,
				modelBounds))
		{
			return false;
		}

		outWorldMatrices.Clear();
		outWorldMatrices.Reserve(modelMatrices.Num());
		for (const glm::mat4& modelMatrix : modelMatrices)
		{
			outWorldMatrices.Add(ownerWorldMatrix * modelMatrix);
		}

		outWorldBounds = modelBounds;
		outWorldBounds.Apply(ownerWorldMatrix);
		return outWorldBounds.IsValid();
	}
}

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
		const ModelPtr& model = data.GetModel();
		const bool bInvalidMeshIndex = model && model->IsReady() &&
			data.GetMeshIndex() != Model::AllMeshes &&
			!model->IsSourceMeshIndexValid(data.GetMeshIndex());
		if (bInvalidMeshIndex && !data.m_bInvalidMeshIndexReported)
		{
			SAILOR_LOG(
				"MeshRenderer ignored invalid glTF mesh index %d for model %s.",
				data.GetMeshIndex(),
				model->GetFileId().ToString().c_str());
			data.m_bInvalidMeshIndexReported = true;
		}
		else if (!bInvalidMeshIndex)
		{
			data.m_bInvalidMeshIndexReported = false;
		}
		const bool bHasRenderableModel = HasRenderableSelection(data);
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
				const bool bHasRenderableModel = HasRenderableSelection(data);
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

					if (mobilityType == EMobilityType::Stationary &&
						data.m_bIsActive && HasRenderableSelection(data))
					{
						const auto& ownerTransform = ownerGameObject->GetTransformComponent();
						Math::AABB adjustedBounds;
						TVector<RHI::RHIMeshPtr> selectedMeshes;
						TVector<glm::mat4> selectedMatrices;

						// Should we update only when transform changed?
						if ((data.m_bIsDirty || data.m_frameLastChange == 0 || ownerTransform.GetFrameLastChange() > data.m_frameLastChange) &&
							CollectComponentRenderData(
								data,
								ownerTransform.GetCachedWorldMatrix(),
								selectedMeshes,
								selectedMatrices,
								adjustedBounds))
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

				if (mobilityType == EMobilityType::Static &&
					data.m_bIsActive && HasRenderableSelection(data))
				{
					const auto& ownerTransform = ownerGameObject->GetTransformComponent();
					Math::AABB adjustedBounds;
					TVector<RHI::RHIMeshPtr> selectedMeshes;
					TVector<glm::mat4> selectedMatrices;
					bool bMaterialsReady = !data.GetMaterials().IsEmpty();
					bool bMaterialsChanged =
						data.m_materialContentRevisions.Num() != data.GetMaterials().Num();
					size_t materialIndex = 0;
					for (const auto& material : data.GetMaterials())
					{
						bMaterialsReady &= material && material->IsReady();
						const uint64_t materialContentRevision =
							material ? material->GetContentRevision() : 0ull;
						if (!bMaterialsChanged &&
							data.m_materialContentRevisions[materialIndex] != materialContentRevision)
						{
							bMaterialsChanged = true;
						}
						++materialIndex;
					}

					if ((data.m_bIsDirty || bMaterialsChanged || ownerTransform.GetFrameLastChange() > data.m_frameLastChange) &&
						bMaterialsReady &&
						CollectComponentRenderData(
							data,
							ownerTransform.GetCachedWorldMatrix(),
							selectedMeshes,
							selectedMatrices,
							adjustedBounds))
					{
						data.m_materialContentRevisions.Resize(data.GetMaterials().Num());
						for (size_t i = 0; i < data.GetMaterials().Num(); ++i)
						{
							data.m_materialContentRevisions[i] = data.GetMaterials()[i]->GetContentRevision();
						}

						RHI::RHISceneViewProxy proxy;
						proxy.m_staticMeshEcs = index;
						proxy.m_worldMatrix = ownerTransform.GetCachedWorldMatrix();
						proxy.m_frame = ownerTransform.GetFrameLastChange();
						if (auto animator = ownerGameObject->GetComponent<AnimatorComponent>())
						{
							data.m_skeletonOffset = animator->GetSkeletonOffset();
						}
						else
						{
							data.m_skeletonOffset = StaticMeshRendererData::InvalidSkeletonOffset;
						}
						proxy.m_skeletonOffset = data.m_skeletonOffset;
						proxy.m_meshes = std::move(selectedMeshes);
						proxy.m_meshModelMatrices = std::move(selectedMatrices);
						proxy.m_worldAabb = adjustedBounds;
						proxy.m_bCastShadows = data.ShouldCastShadow();

						proxy.m_overrideMaterials.Clear();
						proxy.m_renderQueueTags.Clear();
						proxy.m_renderQueueTags.Reserve(proxy.m_meshes.Num());
#if defined(__APPLE__)
						proxy.m_materialTextureSamplers.Clear();
						proxy.m_materialTextureSamplers.Reserve(proxy.m_meshes.Num());
						auto textureImporter = App::GetSubmodule<TextureImporter>();
#endif
						for (size_t i = 0; i < proxy.m_meshes.Num(); i++)
						{
							const size_t materialIndex =
								proxy.m_meshes[i]->ResolveMaterialIndex(
									i, data.GetMaterials().Num());
							auto& material = data.GetMaterials()[materialIndex];
							proxy.m_renderQueueTags.Add(material->GetRenderState().GetTag());
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
