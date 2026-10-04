#include "ECS/PathTracerECS.h"
#include "Engine/GameObject.h"
#include "ECS/TransformECS.h"
#include "ECS/LightingECS.h"
#include "ECS/CameraECS.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "Containers/Hash.h"
#include "Components/MeshRendererComponent.h"
#include "Raytracing/PathTracer.h"
#include <algorithm>
#include <cmath>

using namespace Sailor;

void PathTracerECS::UpdateScene()
{
	SAILOR_PROFILE_FUNCTION();

	auto* pLightingEcs = GetWorld()->GetECS<LightingECS>();
	uint64_t sceneRevision = Fnv1aOffsetBasis;
	HashCombine(sceneRevision, GetWorld(), pLightingEcs->GetLightingRevision());
	bool bRebuild = !m_scene;
	for (const auto& data : m_components)
	{
		if (!data.m_bIsActive || !data.m_options.m_bEnabled || !data.m_owner)
		{
			continue;
		}

		auto* owner = static_cast<GameObject*>(data.m_owner.GetRawPtr());
		auto mesh = owner->GetComponent<MeshRendererComponent>();
		auto model = mesh ? mesh->GetModel() : ModelPtr{};
		if (!model || !model->IsReady())
		{
			continue;
		}

		const int32_t meshIndex = mesh->GetMeshIndex();
		if (!model->HasBLAS(meshIndex) && model->HasCpuMeshes())
		{
			model->BuildBLAS();
		}
		if (!model->HasBLAS(meshIndex))
		{
			continue;
		}

		const auto& materials = mesh->GetMaterials();
		HashCombine(sceneRevision, owner->GetInstanceId(), owner->GetTransformComponent().GetFrameLastChange(),
			model, model->GetBLASGeometry(), meshIndex, materials.Num());
		for (const auto& material : materials)
		{
			HashCombine(sceneRevision, material, material ? material->GetContentRevision() : 0ull);
		}
		bRebuild |= data.m_bIsDirty || data.m_bNeedsRebuild || data.m_options.m_bRebuildEveryFrame;
	}
	if (!bRebuild && m_scene->m_revision == sceneRevision)
	{
		return;
	}

	auto scene = TSharedPtr<RHI::RHIPathTracerScene>::Make();
	scene->m_revision = sceneRevision;
	pLightingEcs->GetLightProxies(scene->m_lights);
	TVector<MaterialPtr> materials;

	for (auto& data : m_components)
	{
		const size_t componentHandle = GetComponentIndex(&data);
		if (!data.m_bIsActive || !data.m_options.m_bEnabled)
		{
			m_proxyOctree.Remove(componentHandle);
			continue;
		}

		GameObjectPtr pOwnerGameObject = data.m_owner.StaticCast<GameObject>();
		if (!pOwnerGameObject)
		{
			m_proxyOctree.Remove(componentHandle);
			continue;
		}

		auto pMeshRenderer = pOwnerGameObject->GetComponent<MeshRendererComponent>();
		ModelPtr pModel = pMeshRenderer ? pMeshRenderer->GetModel() : ModelPtr();
		const int32_t meshIndex = pMeshRenderer ?
			pMeshRenderer->GetMeshIndex() : Model::AllMeshes;
		const FileId modelFileId = pModel ? pModel->GetFileId() : FileId();
		if (modelFileId != data.m_modelFileId ||
			meshIndex != data.m_meshIndex)
		{
			data.m_bNeedsRebuild = true;
		}

		const auto& ownerTransform = pOwnerGameObject->GetTransformComponent();
		const bool bNeedRebuild = data.m_bNeedsRebuild ||
			data.m_bIsDirty ||
			data.m_options.m_bRebuildEveryFrame;

		const bool bTransformChanged = data.m_frameLastChange == 0 ||
			ownerTransform.GetFrameLastChange() > data.m_frameLastChange;
		const bool bNeedsUpdate = bNeedRebuild || bTransformChanged;

		if (bNeedsUpdate)
		{
			data.m_worldMatrix = ownerTransform.GetCachedWorldMatrix();
			data.m_inverseWorldMatrix = glm::inverse(data.m_worldMatrix);
		}

		if (!pModel || !pModel->IsReady() || !pModel->HasBLAS(meshIndex))
		{
			// Model loading is async; keep rebuild pending until BLAS is available.
			data.m_bNeedsRebuild = true;
			data.m_modelFileId = modelFileId;
			data.m_meshIndex = meshIndex;
			if (m_proxyOctree.Contains(componentHandle))
			{
				m_proxyOctree.Remove(componentHandle);
			}
			continue;
		}

		data.m_worldBounds = pModel->GetBoundsAABB(meshIndex);
		data.m_worldBounds.Apply(data.m_worldMatrix);

		if (data.m_worldBounds.IsValid())
		{
			m_proxyOctree.Update(data.m_worldBounds.GetCenter(), data.m_worldBounds.GetExtents(), componentHandle);
		}
		else if (m_proxyOctree.Contains(componentHandle))
		{
			m_proxyOctree.Remove(componentHandle);
		}

		Raytracing::PathTracer::TLASInstance instance{};
		instance.m_model = pModel;
		instance.m_modelGeometry = pModel->GetBLASGeometry();
		instance.m_meshIndex = meshIndex;
		instance.m_worldBounds = data.m_worldBounds;
		instance.m_worldMatrix = data.m_worldMatrix;
		instance.m_inverseWorldMatrix = data.m_inverseWorldMatrix;
		instance.m_materialBaseOffset = static_cast<int32_t>(materials.Num());
		const auto& meshMaterials = pMeshRenderer->GetMaterials();
		if (meshMaterials.IsEmpty())
		{
			materials.Add(MaterialPtr());
		}
		else
		{
			for (const auto& material : meshMaterials)
			{
				materials.Add(material);
			}
		}
		scene->m_instances.Add(std::move(instance));

		if (bNeedsUpdate)
		{
			UpdateGameObject(pOwnerGameObject, GetWorld()->GetCurrentFrame());
			data.m_bNeedsRebuild = false;
			data.m_bIsDirty = false;
			data.m_modelFileId = modelFileId;
			data.m_meshIndex = meshIndex;
			data.m_frameLastChange = ownerTransform.GetFrameLastChange();
		}
	}

	scene->m_materials = Raytracing::PathTracer::CaptureMaterials(materials, &m_materialSnapshots);
	m_scene = std::move(scene);
}

void PathTracerECS::CopySceneView(RHI::RHISceneViewPtr& outSceneView)
{
	SAILOR_PROFILE_FUNCTION();

	if (m_bPathTracingEnabled)
	{
		// Consumer demand is known after Tick; capture on the world owner before dispatch.
		UpdateScene();
		outSceneView->m_pathTracerScene = m_scene;
	}
	else
	{
		outSceneView->m_pathTracerScene.Clear();
	}
}

void PathTracerECS::EndPlay()
{
	ECS::TSystem<PathTracerECS, PathTracerProxyData>::EndPlay();
	m_proxyOctree.Clear();
	m_scene.Clear();
	m_materialSnapshots.Clear();
}
