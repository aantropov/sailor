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

	if (!m_bPathTracingEnabled)
	{
		return nullptr;
	}

	TVector<Raytracing::LightProxy> lightProxies;
	if (auto* pLightingEcs = GetWorld()->GetECS<LightingECS>())
	{
		pLightingEcs->GetLightProxies(lightProxies);
	}

	for (auto& data : m_components)
	{
		if (!data.m_bIsActive || !data.m_options.m_bEnabled)
		{
			const size_t componentHandle = GetComponentIndex(&data);
			if (m_proxyOctree.Contains(componentHandle))
			{
				m_proxyOctree.Remove(componentHandle);
			}
			continue;
		}

		data.SetLightProxies(lightProxies);
		const size_t componentHandle = GetComponentIndex(&data);

		GameObjectPtr pOwnerGameObject = data.m_owner.StaticCast<GameObject>();
		if (!pOwnerGameObject)
		{
			if (m_proxyOctree.Contains(componentHandle))
			{
				m_proxyOctree.Remove(componentHandle);
			}
			continue;
		}

		auto pMeshRenderer = pOwnerGameObject->GetComponent<MeshRendererComponent>();
		const ModelPtr pModel = pMeshRenderer ? pMeshRenderer->GetModel() : ModelPtr();
		const TVector<MaterialPtr>* pLiveMaterials = pMeshRenderer ? &pMeshRenderer->GetMaterials() : nullptr;

		const auto& ownerTransform = pOwnerGameObject->GetTransformComponent();
		const bool bNeedRebuildBLAS = data.m_bNeedsRebuild ||
			data.m_bIsDirty ||
			data.m_options.m_bRebuildEveryFrame;

		const bool bTransformChanged = data.m_frameLastChange == 0 ||
			ownerTransform.GetFrameLastChange() > data.m_frameLastChange;

		if (!bNeedRebuildBLAS && !bTransformChanged)
		{
			continue;
		}

		data.m_worldMatrix = ownerTransform.GetCachedWorldMatrix();
		data.m_inverseWorldMatrix = glm::inverse(data.m_worldMatrix);

		bool bShouldGenerateBLAS = true;
		if (pModel)
		{
			if (auto* pAssetRegistry = App::GetSubmodule<AssetRegistry>())
			{
				if (auto pModelAssetInfo = pAssetRegistry->GetAssetInfoPtr<ModelAssetInfoPtr>(pModel->GetFileId()))
				{
					bShouldGenerateBLAS = pModelAssetInfo->ShouldGenerateBLAS();
				}
			}
		}

		if (!pModel || !bShouldGenerateBLAS || !pModel->IsReady() || !pModel->HasCpuMeshes())
		{
			// Model loading is async; keep rebuild pending until CPU data is ready.
			data.m_bNeedsRebuild = true;
			if (m_proxyOctree.Contains(componentHandle))
			{
				m_proxyOctree.Remove(componentHandle);
			}
			continue;
		}

		if (bNeedRebuildBLAS)
		{
			data.m_worldBounds = Math::AABB();
			data.m_triangles.Clear();
			data.m_bvh.Clear();

			const auto& cpuMeshes = pModel->GetCpuMeshes();
			size_t expectedNumTriangles = 0;
			for (const auto& mesh : cpuMeshes)
			{
				expectedNumTriangles += mesh.m_indices.Num() / 3;
			}
			data.m_triangles.Reserve(expectedNumTriangles);

			for (const auto& mesh : cpuMeshes)
			{
				const int32_t maxMaterialIndex = (pLiveMaterials && pLiveMaterials->Num() > 0) ? ((int32_t)pLiveMaterials->Num() - 1) : 0;
				for (size_t i = 0; i + 2 < mesh.m_indices.Num(); i += 3)
				{
					const uint32_t i0 = mesh.m_indices[i + 0];
					const uint32_t i1 = mesh.m_indices[i + 1];
					const uint32_t i2 = mesh.m_indices[i + 2];

					const auto& v0 = mesh.m_vertices[i0];
					const auto& v1 = mesh.m_vertices[i1];
					const auto& v2 = mesh.m_vertices[i2];

					Math::Triangle tri{};
					tri.m_vertices[0] = v0.m_position;
					tri.m_vertices[1] = v1.m_position;
					tri.m_vertices[2] = v2.m_position;

					tri.m_normals[0] = glm::normalize(v0.m_normal);
					tri.m_normals[1] = glm::normalize(v1.m_normal);
					tri.m_normals[2] = glm::normalize(v2.m_normal);

					tri.m_tangent[0] = glm::normalize(v0.m_tangent);
					tri.m_tangent[1] = glm::normalize(v1.m_tangent);
					tri.m_tangent[2] = glm::normalize(v2.m_tangent);

					tri.m_bitangent[0] = glm::normalize(v0.m_bitangent);
					tri.m_bitangent[1] = glm::normalize(v1.m_bitangent);
					tri.m_bitangent[2] = glm::normalize(v2.m_bitangent);

					tri.m_uvs[0] = v0.m_texcoord;
					tri.m_uvs[1] = v1.m_texcoord;
					tri.m_uvs[2] = v2.m_texcoord;
					tri.m_uvs2[0] = tri.m_uvs[0];
					tri.m_uvs2[1] = tri.m_uvs[1];
					tri.m_uvs2[2] = tri.m_uvs[2];

					const int32_t safeMaterialIndex = (std::max)(0, (std::min)(mesh.m_materialIndex, maxMaterialIndex));
					tri.m_materialIndex = static_cast<uint8_t>((std::max)(0, (std::min)(safeMaterialIndex, 255)));
					tri.m_centroid = (tri.m_vertices[0] + tri.m_vertices[1] + tri.m_vertices[2]) / 3.0f;

					data.m_triangles.Add(tri);
				}
			}

			if (data.m_triangles.Num() > 0)
			{
				data.m_bvh = TSharedPtr<Raytracing::BVH>::Make((uint32_t)data.m_triangles.Num());
				data.m_bvh->BuildBVH(data.m_triangles);
			}
		}

		data.m_worldBounds = Math::AABB();
		for (const auto& tri : data.m_triangles)
		{
			const glm::vec3 v0 = glm::vec3(data.m_worldMatrix * glm::vec4(tri.m_vertices[0], 1.0f));
			const glm::vec3 v1 = glm::vec3(data.m_worldMatrix * glm::vec4(tri.m_vertices[1], 1.0f));
			const glm::vec3 v2 = glm::vec3(data.m_worldMatrix * glm::vec4(tri.m_vertices[2], 1.0f));
			data.m_worldBounds.Extend(v0);
			data.m_worldBounds.Extend(v1);
			data.m_worldBounds.Extend(v2);
		}

		if (data.m_worldBounds.IsValid())
		{
			m_proxyOctree.Update(data.m_worldBounds.GetCenter(), data.m_worldBounds.GetExtents(), componentHandle);
		}
		else if (m_proxyOctree.Contains(componentHandle))
		{
			m_proxyOctree.Remove(componentHandle);
		}

		UpdateGameObject(pOwnerGameObject, GetWorld()->GetCurrentFrame());
		data.m_bNeedsRebuild = false;
		data.m_bIsDirty = false;
		data.m_frameLastChange = ownerTransform.GetFrameLastChange();
	}

	return nullptr;
}

bool PathTracerECS::InitializePathTracer(Raytracing::PathTracer& outPathTracer, size_t componentHandle)
{
	auto appendFromData = [&](PathTracerProxyData& data,
		TVector<Math::Triangle>& inOutWorldTriangles,
		TVector<MaterialPtr>& inOutMaterials,
		int32_t& inOutMaterialOffset) -> bool
	{
		if (!data.m_bIsActive || !data.m_options.m_bEnabled)
		{
			return false;
		}

		if (!data.m_bvh || data.m_triangles.Num() == 0)
		{
			return false;
		}

		GameObjectPtr pOwnerGameObject = data.m_owner.StaticCast<GameObject>();
		auto pMeshRenderer = pOwnerGameObject ? pOwnerGameObject->GetComponent<MeshRendererComponent>() : MeshRendererComponentPtr();
		const TVector<MaterialPtr>* pMaterials = pMeshRenderer ? &pMeshRenderer->GetMaterials() : nullptr;

		const int32_t materialBaseOffset = inOutMaterialOffset;
		if (!pMaterials || pMaterials->Num() == 0)
		{
			// Keep geometry renderable even when model has no bound materials.
			inOutMaterials.Add(MaterialPtr());
			inOutMaterialOffset += 1;
		}
		else
		{
			for (const auto& material : *pMaterials)
			{
				inOutMaterials.Add(material);
			}
			inOutMaterialOffset += (int32_t)pMaterials->Num();
		}

		const glm::mat3 normalMatrix = glm::mat3(glm::transpose(glm::inverse(data.m_worldMatrix)));
		for (const auto& localTri : data.m_triangles)
		{
			Math::Triangle tri = localTri;
			tri.m_vertices[0] = glm::vec3(data.m_worldMatrix * glm::vec4(localTri.m_vertices[0], 1.0f));
			tri.m_vertices[1] = glm::vec3(data.m_worldMatrix * glm::vec4(localTri.m_vertices[1], 1.0f));
			tri.m_vertices[2] = glm::vec3(data.m_worldMatrix * glm::vec4(localTri.m_vertices[2], 1.0f));
			tri.m_normals[0] = glm::normalize(normalMatrix * localTri.m_normals[0]);
			tri.m_normals[1] = glm::normalize(normalMatrix * localTri.m_normals[1]);
			tri.m_normals[2] = glm::normalize(normalMatrix * localTri.m_normals[2]);
			tri.m_tangent[0] = glm::normalize(normalMatrix * localTri.m_tangent[0]);
			tri.m_tangent[1] = glm::normalize(normalMatrix * localTri.m_tangent[1]);
			tri.m_tangent[2] = glm::normalize(normalMatrix * localTri.m_tangent[2]);
			tri.m_bitangent[0] = glm::normalize(normalMatrix * localTri.m_bitangent[0]);
			tri.m_bitangent[1] = glm::normalize(normalMatrix * localTri.m_bitangent[1]);
			tri.m_bitangent[2] = glm::normalize(normalMatrix * localTri.m_bitangent[2]);
			tri.m_materialIndex = static_cast<uint8_t>((std::max)(0, (std::min)(materialBaseOffset + (int32_t)localTri.m_materialIndex, 255)));
			tri.m_centroid = (tri.m_vertices[0] + tri.m_vertices[1] + tri.m_vertices[2]) / 3.0f;
			inOutWorldTriangles.Add(tri);
		}

		return true;
	};

	TVector<Raytracing::LightProxy> lights;
	if (auto* pLightingEcs = GetWorld()->GetECS<LightingECS>())
	{
		pLightingEcs->GetLightProxies(lights);
	}

	if (componentHandle != (size_t)-1 && componentHandle < m_components.Num())
	{
		TVector<Math::Triangle> worldTriangles;
		TVector<MaterialPtr> materials;
		int32_t materialOffset = 0;
		if (!appendFromData(m_components[componentHandle], worldTriangles, materials, materialOffset))
		{
			return false;
		}
		return outPathTracer.InitializeScene(worldTriangles, materials, lights);
	}

	TVector<Math::Triangle> worldTriangles;
	TVector<MaterialPtr> materials;
	int32_t materialOffset = 0;
	bool bHasAny = false;
	for (auto& data : m_components)
	{
		bHasAny |= appendFromData(data, worldTriangles, materials, materialOffset);
	}

	return bHasAny && outPathTracer.InitializeScene(worldTriangles, materials, lights);
}

bool PathTracerECS::RenderScene(const Raytracing::PathTracer::Params& params, size_t componentHandle)
{
	Raytracing::PathTracer::Params runtimeParams = params;

	if (runtimeParams.m_msaa == 0)
	{
		runtimeParams.m_msaa = 1;
	}

	if (runtimeParams.m_numSamples == 0)
	{
		runtimeParams.m_numSamples = 16;
	}

	if (runtimeParams.m_numAmbientSamples == 0)
	{
		runtimeParams.m_numAmbientSamples = 16;
	}

	if (runtimeParams.m_ambient == glm::vec3(0.0f))
	{
		runtimeParams.m_ambient = glm::vec3(0.03f);
	}

	if (runtimeParams.m_maxBounces == 0)
	{
		runtimeParams.m_maxBounces = 4;
	}

	if (componentHandle != (size_t)-1 && componentHandle < m_components.Num())
	{
		const auto& options = m_components[componentHandle].m_options;
		if (params.m_maxBounces == 0)
		{
			runtimeParams.m_maxBounces = (std::max)(1u, options.m_maxBounces);
		}

		if (params.m_numSamples == 0 && params.m_msaa == 0)
		{
			const uint32_t samplesPerPixel = (std::max)(1u, options.m_samplesPerPixel);
			runtimeParams.m_msaa = samplesPerPixel <= 32 ? (std::min)(4u, samplesPerPixel) : 8u;
			runtimeParams.m_numSamples = (std::max)(1u, (uint32_t)std::lround(samplesPerPixel / (float)runtimeParams.m_msaa));
		}
	}

	if (auto* pCameraEcs = GetWorld()->GetECS<CameraECS>())
	{
		Math::Transform cameraTransform{};
		CameraData cameraData{};
		if (pCameraEcs->TryGetActiveCamera(cameraTransform, cameraData))
		{
			runtimeParams.m_bUseRuntimeCamera = true;
			runtimeParams.m_runtimeCameraPos = glm::vec3(cameraTransform.m_position);
			runtimeParams.m_runtimeCameraForward = glm::normalize(cameraTransform.GetForward());
			runtimeParams.m_runtimeCameraUp = glm::normalize(cameraTransform.GetUp());
			runtimeParams.m_runtimeAspectRatio = cameraData.GetAspect();
			runtimeParams.m_runtimeHFov = glm::radians(cameraData.GetFov());
		}
	}

	const bool bWasEnabled = m_bPathTracingEnabled;
	m_bPathTracingEnabled = true;
	Tick(0.0f);
	m_bPathTracingEnabled = bWasEnabled;

	if (!InitializePathTracer(m_pathTracer, componentHandle))
	{
		return false;
	}

	return m_pathTracer.RenderPreparedScene(runtimeParams);
}

bool PathTracerECS::IntersectProxyRay(size_t componentHandle, const Math::Ray& worldRay, Math::RaycastHit& outHit, float maxRayLength) const
{
	outHit = Math::RaycastHit();

	if (componentHandle >= m_components.Num())
	{
		return false;
	}

	const auto& data = m_components[componentHandle];
	if (!data.m_bIsActive || !data.m_options.m_bEnabled || !data.m_bvh || !data.m_worldBounds.IsValid())
	{
		return false;
	}

	if (Math::IntersectRayAABB(worldRay, data.m_worldBounds.m_min, data.m_worldBounds.m_max, maxRayLength) == FLT_MAX)
	{
		return false;
	}

	const glm::vec3 localOrigin = glm::vec3(data.m_inverseWorldMatrix * glm::vec4(worldRay.GetOrigin(), 1.0f));
	const glm::vec3 localDirection = glm::normalize(glm::vec3(data.m_inverseWorldMatrix * glm::vec4(worldRay.GetDirection(), 0.0f)));

	Math::Ray localRay(localOrigin, localDirection);
	Math::RaycastHit localHit{};
	if (!data.m_bvh->IntersectBVH(localRay, localHit, 0, maxRayLength))
	{
		return false;
	}

	outHit = localHit;
	outHit.m_point = glm::vec3(data.m_worldMatrix * glm::vec4(localHit.m_point, 1.0f));
	const glm::mat3 normalMatrix = glm::mat3(glm::transpose(data.m_inverseWorldMatrix));
	outHit.m_normal = glm::normalize(normalMatrix * localHit.m_normal);
	outHit.m_rayLenght = glm::length(outHit.m_point - worldRay.GetOrigin());

	return outHit.HasIntersection() && outHit.m_rayLenght <= maxRayLength;
}

bool PathTracerECS::IntersectSceneRay(const Math::Ray& worldRay, Math::RaycastHit& outHit, size_t& outComponentHandle, float maxRayLength) const
{
	outHit = Math::RaycastHit();
	outComponentHandle = ECS::InvalidIndex;

	TVector<size_t> candidateComponents;
	m_proxyOctree.TraceRay(worldRay, candidateComponents, maxRayLength);
	if (candidateComponents.Num() == 0)
	{
		return false;
	}

	Math::RaycastHit bestHit{};
	float bestDistance = maxRayLength;

	for (const size_t componentHandle : candidateComponents)
	{
		Math::RaycastHit hit{};
		if (!IntersectProxyRay(componentHandle, worldRay, hit, bestDistance))
		{
			continue;
		}

		if (hit.m_rayLenght < bestDistance)
		{
			bestDistance = hit.m_rayLenght;
			bestHit = hit;
			outComponentHandle = componentHandle;
		}
	}

	if (outComponentHandle == ECS::InvalidIndex)
	{
		return false;
	}

	outHit = bestHit;
	return true;
}
