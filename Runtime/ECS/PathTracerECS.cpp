#include "ECS/PathTracerECS.h"
#include "Engine/GameObject.h"
#include "ECS/TransformECS.h"
#include "ECS/LightingECS.h"
#include "ECS/CameraECS.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "Components/MeshRendererComponent.h"
#include "Raytracing/PathTracer.h"
#include <algorithm>
#include <cmath>

using namespace Sailor;

Tasks::ITaskPtr PathTracerECS::Tick(float deltaTime)
{
	SAILOR_PROFILE_FUNCTION();

	m_pathTracerProxiesCache.Clear();
	m_pathTracerTLASInstancesCache.Clear();
	m_pathTracerMaterialsCache.Clear();
	m_pathTracerLightsCache.Clear();

	auto* pLightingEcs = GetWorld()->GetECS<LightingECS>();
	pLightingEcs->GetLightProxies(m_pathTracerLightsCache);

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
		const FileId modelFileId = pModel ? pModel->GetFileId() : FileId();
		if (modelFileId != data.m_modelFileId)
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

		if (bNeedsUpdate && pModel && !pModel->HasBLAS() && pModel->HasCpuMeshes())
		{
			pModel->BuildBLAS();
		}

		if (!pModel || !pModel->IsReady() || !pModel->HasBLAS())
		{
			// Model loading is async; keep rebuild pending until BLAS is available.
			data.m_bNeedsRebuild = true;
			data.m_modelFileId = modelFileId;
			if (m_proxyOctree.Contains(componentHandle))
			{
				m_proxyOctree.Remove(componentHandle);
			}
			continue;
		}

		data.m_worldBounds = pModel->GetBoundsAABB();
		data.m_worldBounds.Apply(data.m_worldMatrix);

		if (data.m_worldBounds.IsValid())
		{
			m_proxyOctree.Update(data.m_worldBounds.GetCenter(), data.m_worldBounds.GetExtents(), componentHandle);
		}
		else if (m_proxyOctree.Contains(componentHandle))
		{
			m_proxyOctree.Remove(componentHandle);
		}

		RHI::RHIPathTracerProxy proxy{};
		proxy.m_model = pModel;
		proxy.m_worldBounds = data.m_worldBounds;
		proxy.m_worldMatrix = data.m_worldMatrix;
		proxy.m_inverseWorldMatrix = data.m_inverseWorldMatrix;
		proxy.m_frameLastChange = data.m_frameLastChange;
		if (pMeshRenderer)
		{
			proxy.m_materials = pMeshRenderer->GetMaterials();
		}
		m_pathTracerProxiesCache.Add(proxy);

		Raytracing::PathTracer::TLASInstance instance{};
		instance.m_model = pModel;
		instance.m_worldBounds = data.m_worldBounds;
		instance.m_worldMatrix = data.m_worldMatrix;
		instance.m_inverseWorldMatrix = data.m_inverseWorldMatrix;
		instance.m_materialBaseOffset = (int32_t)m_pathTracerMaterialsCache.Num();
		if (proxy.m_materials.Num() == 0)
		{
			m_pathTracerMaterialsCache.Add(MaterialPtr());
		}
		else
		{
			for (const auto& material : proxy.m_materials)
			{
				m_pathTracerMaterialsCache.Add(material);
			}
		}
		m_pathTracerTLASInstancesCache.Add(std::move(instance));

		if (bNeedsUpdate)
		{
			UpdateGameObject(pOwnerGameObject, GetWorld()->GetCurrentFrame());
			data.m_bNeedsRebuild = false;
			data.m_bIsDirty = false;
			data.m_modelFileId = modelFileId;
			data.m_frameLastChange = ownerTransform.GetFrameLastChange();
		}
	}

	return nullptr;
}

void PathTracerECS::CopySceneView(RHI::RHISceneViewPtr& outSceneView)
{
	SAILOR_PROFILE_FUNCTION();

	outSceneView->m_pathTracerProxies = m_pathTracerProxiesCache;
	outSceneView->m_pathTracerTLASInstances = m_pathTracerTLASInstancesCache;
	outSceneView->m_pathTracerMaterials = m_pathTracerMaterialsCache;
	outSceneView->m_pathTracerLights = m_pathTracerLightsCache;
}


