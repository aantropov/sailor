#include "SceneView.h"
#include "ECS/CameraECS.h"
#include "ECS/TransformECS.h"
#include "ECS/StaticMeshRendererECS.h"
#include "Engine/GameObject.h"
#include "Math/Transform.h"
#include "Engine/World.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "RHI/DebugContext.h"
#include "RHI/CommandList.h"
#include "Tasks/Scheduler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

using namespace Sailor;
using namespace Sailor::RHI;

float Sailor::RHI::CalculateScreenCoverage(
	const Math::AABB& worldBounds,
	const glm::mat4& viewMatrix,
	const glm::mat4& projectionMatrix)
{
	const auto isMatrixFinite = [](const glm::mat4& matrix)
		{
			return Math::AllFinite(matrix[0]) &&
				Math::AllFinite(matrix[1]) &&
				Math::AllFinite(matrix[2]) &&
				Math::AllFinite(matrix[3]);
		};
	if (!worldBounds.IsValid() ||
		!isMatrixFinite(viewMatrix) ||
		!isMatrixFinite(projectionMatrix))
	{
		return 0.0f;
	}

	const glm::vec3 min = worldBounds.m_min;
	const glm::vec3 max = worldBounds.m_max;
	const std::array<glm::vec3, 8u> corners = {
		glm::vec3(min.x, min.y, min.z),
		glm::vec3(max.x, min.y, min.z),
		glm::vec3(min.x, max.y, min.z),
		glm::vec3(max.x, max.y, min.z),
		glm::vec3(min.x, min.y, max.z),
		glm::vec3(max.x, min.y, max.z),
		glm::vec3(min.x, max.y, max.z),
		glm::vec3(max.x, max.y, max.z)
	};

	const glm::mat4 viewProjection = projectionMatrix * viewMatrix;
	std::array<glm::vec4, 8u> clipCorners{};
	uint32_t numCornersInFront = 0u;
	for (size_t cornerIndex = 0u;
		cornerIndex < corners.size();
		++cornerIndex)
	{
		glm::vec4& clip = clipCorners[cornerIndex];
		clip = viewProjection * glm::vec4(corners[cornerIndex], 1.0f);
		if (!Math::AllFinite(clip))
		{
			return 0.0f;
		}
		numCornersInFront += clip.w > 1e-5f ? 1u : 0u;
	}
	if (numCornersInFront == 0u)
	{
		return 0.0f;
	}
	if (numCornersInFront < corners.size())
	{
		return 1.0f;
	}

	glm::vec2 minNdc((std::numeric_limits<float>::max)());
	glm::vec2 maxNdc((std::numeric_limits<float>::lowest)());
	for (const glm::vec4& clip : clipCorners)
	{
		const glm::vec2 ndc = glm::vec2(clip) / clip.w;
		minNdc = glm::min(minNdc, ndc);
		maxNdc = glm::max(maxNdc, ndc);
	}

	minNdc = glm::max(minNdc, glm::vec2(-1.0f));
	maxNdc = glm::min(maxNdc, glm::vec2(1.0f));
	const glm::vec2 coveredNdc = glm::max(
		maxNdc - minNdc,
		glm::vec2(0.0f));
	return (std::clamp)(
		coveredNdc.x * coveredNdc.y * 0.25f,
		0.0f,
		1.0f);
}

uint32_t RHI::RHILodPolicy::Resolve(float screenCoverage, uint32_t numAvailableLods) const
{
	if (numAvailableLods == 0u)
	{
		return 0u;
	}

	const uint32_t highestAvailableLod = numAvailableLods - 1u;
	const uint32_t minLod = (std::min)(m_minLod, highestAvailableLod);
	const uint32_t maxLod = (std::max)(minLod, (std::min)(m_maxLod, highestAvailableLod));
	uint32_t selectedLod = 0u;
	const float coverage = (std::clamp)(screenCoverage, 0.0f, 1.0f);
	for (size_t index = 0u; index < m_screenCoverageThresholds.Num(); ++index)
	{
		if (!std::isfinite(m_screenCoverageThresholds[index]) ||
			coverage >= m_screenCoverageThresholds[index])
		{
			break;
		}
		selectedLod = static_cast<uint32_t>(index + 1u);
	}
	return (std::clamp)(selectedLod, minLod, maxLod);
}

bool RHI::RHIInstanceLodCullingData::IsValid(size_t numMeshes) const
{
	if (!m_cellWorldAabb.IsValid() ||
		m_meshesPerInstance == 0u ||
		!m_instanceWorldBounds ||
		m_instanceWorldBounds->m_aabbs.IsEmpty() ||
		!Math::AllFinite(m_minInstanceWorldExtents) ||
		!Math::AllFinite(m_maxInstanceWorldExtents) ||
		glm::any(glm::lessThan(m_minInstanceWorldExtents, glm::vec3(0.0f))) ||
		glm::any(glm::lessThan(m_maxInstanceWorldExtents, m_minInstanceWorldExtents)))
	{
		return false;
	}

	const size_t meshesPerInstance = static_cast<size_t>(m_meshesPerInstance);
	const size_t numInstances = m_instanceWorldBounds->m_aabbs.Num();
	return numInstances <= numMeshes / meshesPerInstance &&
		numInstances * meshesPerInstance == numMeshes;
}

namespace
{
	template<typename T>
	bool IsAlignedOrEmpty(const TVector<T>& values, size_t expectedSize)
	{
		return values.IsEmpty() || values.Num() == expectedSize;
	}

	template<typename T>
	void MoveAlignedElement(TVector<T>& values,
		size_t sourceIndex,
		size_t destinationIndex)
	{
		if (!values.IsEmpty() && sourceIndex != destinationIndex)
		{
			values[destinationIndex] = std::move(values[sourceIndex]);
		}
	}

	template<typename T>
	void ResizeAligned(TVector<T>& values, size_t size)
	{
		if (!values.IsEmpty())
		{
			values.Resize(size);
		}
	}

	std::array<glm::vec3, 8u> GetAabbPoints(const Math::AABB& bounds)
	{
		const glm::vec3& min = bounds.m_min;
		const glm::vec3& max = bounds.m_max;
		return {
			glm::vec3(min.x, min.y, min.z),
			glm::vec3(max.x, max.y, max.z),
			glm::vec3(min.x, max.y, max.z),
			glm::vec3(max.x, min.y, max.z),
			glm::vec3(max.x, max.y, min.z),
			glm::vec3(max.x, min.y, min.z),
			glm::vec3(min.x, max.y, min.z),
			glm::vec3(min.x, min.y, max.z)
		};
	}

	RHIMeshPtr ResolveLodMesh(const RHILodPolicy& policy,
		float screenCoverage,
		const RHIMeshPtr& sourceMesh)
	{
		if (!sourceMesh)
		{
			return {};
		}

		const uint32_t lod = policy.Resolve(
			screenCoverage,
			sourceMesh->GetNumLods());
		if (lod > 0u)
		{
			if (RHIMeshPtr lodMesh = sourceMesh->GetLod(lod))
			{
				return lodMesh;
			}
		}
		return sourceMesh;
	}

	void ClearProxyMeshData(RHISceneViewProxy& proxy)
	{
		proxy.m_meshes.Clear();
		proxy.m_meshModelMatrices.Clear();
		proxy.m_overrideMaterials.Clear();
		proxy.m_renderQueueTags.Clear();
		proxy.m_baseColorFactors.Clear();
		proxy.m_baseColorSamplers.Clear();
		proxy.m_alphaCutoffs.Clear();
#if defined(__APPLE__)
		proxy.m_materialTextureSamplers.Clear();
#endif
		proxy.m_instanceLodCulling = {};
		proxy.m_worldAabb = {};
	}

	uint32_t ResolveProxyLod(
		const StaticMeshRendererData& data,
		const Math::AABB& worldBounds,
		const CameraData& camera,
		const RHIMeshPtr& mesh)
	{
		if (!mesh)
		{
			return 0u;
		}

		const float screenCoverage = CalculateScreenCoverage(
			worldBounds,
			camera.GetViewMatrix(),
			camera.GetProjectionMatrix());
		return data.ResolveLod(screenCoverage, mesh->GetNumLods());
	}

	void ApplyCustomLodToMeshes(
		const RHILodPolicy& policy,
		const Math::AABB& worldBounds,
		const CameraData& camera,
		TVector<RHIMeshPtr>& meshes)
	{
		const float coverage = CalculateScreenCoverage(
			worldBounds, camera.GetViewMatrix(), camera.GetProjectionMatrix());
		for (auto& mesh : meshes)
		{
			if (!mesh)
			{
				continue;
			}
			const uint32_t lod = policy.Resolve(coverage, mesh->GetNumLods());
			if (lod > 0u)
			{
				if (RHIMeshPtr lodMesh = mesh->GetLod(lod))
				{
					mesh = std::move(lodMesh);
				}
			}
		}
	}

	void ApplyLodToMeshes(
		const StaticMeshRendererData& data,
		const Math::AABB& worldBounds,
		const CameraData& camera,
		TVector<RHIMeshPtr>& meshes)
	{
		for (auto& mesh : meshes)
		{
			const uint32_t lod = ResolveProxyLod(
				data,
				worldBounds,
				camera,
				mesh);
			if (lod > 0u)
			{
				if (RHIMeshPtr lodMesh = mesh->GetLod(lod))
				{
					mesh = std::move(lodMesh);
				}
			}
		}
	}

	RHIInstanceLodCullingDecision ClassifyInstanceCellData(
		const Math::AABB& worldAabb,
		const RHILodPolicy& lodPolicy,
		const RHIInstanceLodCullingData& instanceData,
		size_t numMeshes,
		bool bHasAlignedInstanceData,
		const Math::Frustum& frustum,
		const glm::vec3& cameraPosition,
		const glm::mat4& viewMatrix,
		const glm::mat4& projectionMatrix)
	{
		RHIInstanceLodCullingDecision decision;
		if (!bHasAlignedInstanceData ||
			!instanceData.IsValid(numMeshes))
		{
			decision.m_uniformScreenCoverage = CalculateScreenCoverage(
				worldAabb,
				viewMatrix,
				projectionMatrix);
			return decision;
		}

		if (!worldAabb.IsValid() || !frustum.OverlapsAABB(worldAabb))
		{
			decision.m_mode = EInstanceLodCullingMode::Culled;
			return decision;
		}

		const auto cullingCellPoints = GetAabbPoints(worldAabb);
		bool bCrossesBoundary = false;
		if (std::isfinite(lodPolicy.m_maxCameraDistance))
		{
			const glm::vec3 closestPoint = glm::clamp(
				cameraPosition,
				worldAabb.m_min,
				worldAabb.m_max);
			const float nearestDistance = glm::distance(
				cameraPosition,
				closestPoint);
			if (nearestDistance > lodPolicy.m_maxCameraDistance)
			{
				decision.m_mode = EInstanceLodCullingMode::Culled;
				return decision;
			}

			float farthestVertexDistance = 0.0f;
			for (const glm::vec3& point : cullingCellPoints)
			{
				farthestVertexDistance = (std::max)(
					farthestVertexDistance,
					glm::distance(cameraPosition, point));
			}
			bCrossesBoundary =
				farthestVertexDistance > lodPolicy.m_maxCameraDistance;
		}

		for (const glm::vec3& point : cullingCellPoints)
		{
			if (!frustum.ContainsPoint(point))
			{
				bCrossesBoundary = true;
				break;
			}
		}

		if (bCrossesBoundary)
		{
			decision.m_mode = EInstanceLodCullingMode::PerInstance;
			return decision;
		}

		float minScreenCoverage = 1.0f;
		float maxScreenCoverage = 0.0f;
		const auto cellPoints = GetAabbPoints(instanceData.m_cellWorldAabb);
		const auto sampleCoverage = [&](const glm::vec3& center,
			const glm::vec3& extents)
			{
				const float coverage = CalculateScreenCoverage(
					Math::AABB(center, glm::max(extents, glm::vec3(0.001f))),
					viewMatrix,
					projectionMatrix);
				minScreenCoverage = (std::min)(minScreenCoverage, coverage);
				maxScreenCoverage = (std::max)(maxScreenCoverage, coverage);
			};

		for (const glm::vec3& point : cellPoints)
		{
			sampleCoverage(point, instanceData.m_minInstanceWorldExtents);
			sampleCoverage(point, instanceData.m_maxInstanceWorldExtents);
		}
		const glm::vec3 closestCellPoint = glm::clamp(
			cameraPosition,
			instanceData.m_cellWorldAabb.m_min,
			instanceData.m_cellWorldAabb.m_max);
		sampleCoverage(closestCellPoint,
			instanceData.m_minInstanceWorldExtents);
		sampleCoverage(closestCellPoint,
			instanceData.m_maxInstanceWorldExtents);
		sampleCoverage(instanceData.m_cellWorldAabb.GetCenter(),
			instanceData.m_minInstanceWorldExtents);
		sampleCoverage(instanceData.m_cellWorldAabb.GetCenter(),
			instanceData.m_maxInstanceWorldExtents);

		decision.m_uniformScreenCoverage =
			(minScreenCoverage + maxScreenCoverage) * 0.5f;
		if (lodPolicy.m_bEnabled)
		{
			const size_t policyLodCount = (std::max)(
				static_cast<size_t>(lodPolicy.m_maxLod) + 1u,
				lodPolicy.m_screenCoverageThresholds.Num() + 1u);
			const uint32_t numAvailableLods = static_cast<uint32_t>((std::min)(
				policyLodCount,
				static_cast<size_t>((std::numeric_limits<uint32_t>::max)())));
			if (lodPolicy.Resolve(minScreenCoverage, numAvailableLods) !=
				lodPolicy.Resolve(maxScreenCoverage, numAvailableLods))
			{
				decision.m_mode = EInstanceLodCullingMode::PerInstance;
			}
		}

		return decision;
	}

	RHIShadowCasterProxyPtr CreateLodShadowCaster(
		const RHIShadowCasterProxyPtr& source,
		WorldPtr world,
		const CameraData& camera,
		const Math::Transform& cameraTransform,
		const Math::Frustum* cullingFrustum = nullptr)
	{
		if (!source || !world)
		{
			return source;
		}
		if (cullingFrustum &&
			SceneViewDetails::HasAlignedInstanceData(*source))
		{
			const auto decision = SceneViewDetails::ClassifyInstanceCell(
				*source,
				*cullingFrustum,
				glm::vec3(cameraTransform.m_position),
				camera.GetViewMatrix(),
				camera.GetProjectionMatrix());
			return SceneViewDetails::ApplyInstanceLodCullingDecision(
				source,
				decision,
				*cullingFrustum,
				glm::vec3(cameraTransform.m_position),
				camera.GetViewMatrix(),
				camera.GetProjectionMatrix());
		}

		auto* meshEcs = world->GetECS<StaticMeshRendererECS>();
		const bool bUseStaticMeshSettings = meshEcs &&
			meshEcs->IsComponentRegistered(source->m_staticMeshEcs);
		if (!source->m_lodPolicy.m_bEnabled && !bUseStaticMeshSettings)
		{
			return source;
		}

		RHIShadowCasterProxyPtr result = RHIShadowCasterProxyPtr::Make(*source);
		const float coverage = CalculateScreenCoverage(
			source->m_worldAabb, camera.GetViewMatrix(), camera.GetProjectionMatrix());
		for (auto& shadowMesh : result->m_meshes)
		{
			const uint32_t lod = source->m_lodPolicy.m_bEnabled ?
				source->m_lodPolicy.Resolve(coverage,
					shadowMesh.m_mesh ? shadowMesh.m_mesh->GetNumLods() : 0u) :
				ResolveProxyLod(meshEcs->GetComponentData(source->m_staticMeshEcs),
					source->m_worldAabb, camera, shadowMesh.m_mesh);
			if (lod > 0u)
			{
				if (RHIMeshPtr lodMesh = shadowMesh.m_mesh->GetLod(lod))
				{
					shadowMesh.m_mesh = std::move(lodMesh);
				}
			}
		}
		return result;
	}
}

bool RHI::SceneViewDetails::HasAlignedInstanceData(const RHISceneViewProxy& proxy)
{
	const size_t numMeshes = proxy.m_meshes.Num();
	if (!proxy.m_instanceLodCulling.IsValid(numMeshes))
	{
		return false;
	}

	return IsAlignedOrEmpty(proxy.m_meshModelMatrices, numMeshes) &&
		IsAlignedOrEmpty(proxy.m_overrideMaterials, numMeshes) &&
		IsAlignedOrEmpty(proxy.m_renderQueueTags, numMeshes) &&
		IsAlignedOrEmpty(proxy.m_baseColorFactors, numMeshes) &&
		IsAlignedOrEmpty(proxy.m_baseColorSamplers, numMeshes) &&
		IsAlignedOrEmpty(proxy.m_alphaCutoffs, numMeshes)
#if defined(__APPLE__)
		&& IsAlignedOrEmpty(proxy.m_materialTextureSamplers, numMeshes)
#endif
		;
}

bool RHI::SceneViewDetails::HasAlignedInstanceData(
	const RHIShadowCasterProxy& proxy)
{
	return proxy.m_instanceLodCulling.IsValid(proxy.m_meshes.Num());
}

RHIInstanceLodCullingDecision RHI::SceneViewDetails::ClassifyInstanceCell(
	const RHISceneViewProxy& proxy,
	const Math::Frustum& frustum,
	const glm::vec3& cameraPosition,
	const glm::mat4& viewMatrix,
	const glm::mat4& projectionMatrix)
{
	return ClassifyInstanceCellData(
		proxy.m_worldAabb,
		proxy.m_lodPolicy,
		proxy.m_instanceLodCulling,
		proxy.m_meshes.Num(),
		HasAlignedInstanceData(proxy),
		frustum,
		cameraPosition,
		viewMatrix,
		projectionMatrix);
}

RHIInstanceLodCullingDecision RHI::SceneViewDetails::ClassifyInstanceCell(
	const RHIShadowCasterProxy& proxy,
	const Math::Frustum& frustum,
	const glm::vec3& cameraPosition,
	const glm::mat4& viewMatrix,
	const glm::mat4& projectionMatrix)
{
	return ClassifyInstanceCellData(
		proxy.m_worldAabb,
		proxy.m_lodPolicy,
		proxy.m_instanceLodCulling,
		proxy.m_meshes.Num(),
		HasAlignedInstanceData(proxy),
		frustum,
		cameraPosition,
		viewMatrix,
		projectionMatrix);
}

bool RHI::SceneViewDetails::ApplyInstanceLodCullingDecision(
	RHISceneViewProxy& proxy,
	const RHIInstanceLodCullingDecision& decision,
	const Math::Frustum& frustum,
	const glm::vec3& cameraPosition,
	const glm::mat4& viewMatrix,
	const glm::mat4& projectionMatrix)
{
	if (!HasAlignedInstanceData(proxy))
	{
		return !proxy.m_meshes.IsEmpty();
	}

	if (decision.m_mode == EInstanceLodCullingMode::Culled)
	{
		ClearProxyMeshData(proxy);
		return false;
	}

	if (decision.m_mode == EInstanceLodCullingMode::Uniform)
	{
		if (proxy.m_lodPolicy.m_bEnabled)
		{
			for (RHIMeshPtr& mesh : proxy.m_meshes)
			{
				mesh = ResolveLodMesh(
					proxy.m_lodPolicy,
					decision.m_uniformScreenCoverage,
					mesh);
			}
		}
		proxy.m_instanceLodCulling = {};
		return !proxy.m_meshes.IsEmpty();
	}

	const size_t meshesPerInstance = static_cast<size_t>(
		proxy.m_instanceLodCulling.m_meshesPerInstance);
	const TSharedPtr<RHIInstanceWorldBounds> sourceInstanceWorldBounds =
		proxy.m_instanceLodCulling.m_instanceWorldBounds;
	const TVector<Math::AABB>& sourceInstanceWorldAabbs =
		sourceInstanceWorldBounds->m_aabbs;

	// Compact every mesh-parallel stream in place to keep indices aligned
	// without allocating a second full set of vegetation instance data.
	Math::AABB visibleWorldAabb;
	size_t writeMeshIndex = 0u;
	for (size_t instanceIndex = 0u;
		instanceIndex < sourceInstanceWorldAabbs.Num();
		++instanceIndex)
	{
		const Math::AABB& instanceWorldAabb =
			sourceInstanceWorldAabbs[instanceIndex];
		if (!instanceWorldAabb.IsValid())
		{
			continue;
		}

		if (std::isfinite(proxy.m_lodPolicy.m_maxCameraDistance))
		{
			const glm::vec3 closestPoint = glm::clamp(
				cameraPosition,
				instanceWorldAabb.m_min,
				instanceWorldAabb.m_max);
			if (glm::distance(cameraPosition, closestPoint) >
				proxy.m_lodPolicy.m_maxCameraDistance)
			{
				continue;
			}
		}
		if (!frustum.OverlapsAABB(instanceWorldAabb))
		{
			continue;
		}

		const size_t firstMesh = instanceIndex * meshesPerInstance;
		const size_t endMesh = firstMesh + meshesPerInstance;
		bool bMeshesValid = endMesh <= proxy.m_meshes.Num();
		for (size_t meshIndex = firstMesh;
			bMeshesValid && meshIndex < endMesh;
			++meshIndex)
		{
			bMeshesValid = proxy.m_meshes[meshIndex].IsValid();
		}
		if (!bMeshesValid)
		{
			continue;
		}

		const float screenCoverage = proxy.m_lodPolicy.m_bEnabled ?
			CalculateScreenCoverage(instanceWorldAabb,
				viewMatrix,
				projectionMatrix) : 0.0f;
		for (size_t meshIndex = firstMesh;
			meshIndex < endMesh;
			++meshIndex)
		{
			RHIMeshPtr resolvedMesh = ResolveLodMesh(
				proxy.m_lodPolicy,
				screenCoverage,
				proxy.m_meshes[meshIndex]);
			proxy.m_meshes[writeMeshIndex] = std::move(resolvedMesh);
			MoveAlignedElement(proxy.m_meshModelMatrices,
				meshIndex,
				writeMeshIndex);
			MoveAlignedElement(proxy.m_overrideMaterials,
				meshIndex,
				writeMeshIndex);
			MoveAlignedElement(proxy.m_renderQueueTags,
				meshIndex,
				writeMeshIndex);
			MoveAlignedElement(proxy.m_baseColorFactors,
				meshIndex,
				writeMeshIndex);
			MoveAlignedElement(proxy.m_baseColorSamplers,
				meshIndex,
				writeMeshIndex);
			MoveAlignedElement(proxy.m_alphaCutoffs,
				meshIndex,
				writeMeshIndex);
#if defined(__APPLE__)
			MoveAlignedElement(proxy.m_materialTextureSamplers,
				meshIndex,
				writeMeshIndex);
#endif
			++writeMeshIndex;
		}

		visibleWorldAabb.Extend(instanceWorldAabb);
	}

	if (writeMeshIndex == 0u)
	{
		ClearProxyMeshData(proxy);
		return false;
	}

	proxy.m_meshes.Resize(writeMeshIndex);
	ResizeAligned(proxy.m_meshModelMatrices, writeMeshIndex);
	ResizeAligned(proxy.m_overrideMaterials, writeMeshIndex);
	ResizeAligned(proxy.m_renderQueueTags, writeMeshIndex);
	ResizeAligned(proxy.m_baseColorFactors, writeMeshIndex);
	ResizeAligned(proxy.m_baseColorSamplers, writeMeshIndex);
	ResizeAligned(proxy.m_alphaCutoffs, writeMeshIndex);
#if defined(__APPLE__)
	ResizeAligned(proxy.m_materialTextureSamplers, writeMeshIndex);
#endif
	proxy.m_instanceLodCulling = {};
	proxy.m_worldAabb = visibleWorldAabb;
	return true;
}

RHIShadowCasterProxyPtr RHI::SceneViewDetails::ApplyInstanceLodCullingDecision(
	const RHIShadowCasterProxyPtr& source,
	const RHIInstanceLodCullingDecision& decision,
	const Math::Frustum& frustum,
	const glm::vec3& cameraPosition,
	const glm::mat4& viewMatrix,
	const glm::mat4& projectionMatrix)
{
	if (!source || !HasAlignedInstanceData(*source))
	{
		return source;
	}
	if (decision.m_mode == EInstanceLodCullingMode::Culled)
	{
		return {};
	}

	RHIShadowCasterProxyPtr result = RHIShadowCasterProxyPtr::Make(*source);
	if (decision.m_mode == EInstanceLodCullingMode::Uniform)
	{
		if (result->m_lodPolicy.m_bEnabled)
		{
			for (RHIShadowMeshProxy& shadowMesh : result->m_meshes)
			{
				shadowMesh.m_mesh = ResolveLodMesh(
					result->m_lodPolicy,
					decision.m_uniformScreenCoverage,
					shadowMesh.m_mesh);
			}
		}
		result->m_instanceLodCulling = {};
		return result;
	}

	const size_t meshesPerInstance = static_cast<size_t>(
		result->m_instanceLodCulling.m_meshesPerInstance);
	const TSharedPtr<RHIInstanceWorldBounds> sourceInstanceWorldBounds =
		result->m_instanceLodCulling.m_instanceWorldBounds;
	const TVector<Math::AABB>& sourceInstanceWorldAabbs =
		sourceInstanceWorldBounds->m_aabbs;

	Math::AABB visibleWorldAabb;
	size_t writeMeshIndex = 0u;
	for (size_t instanceIndex = 0u;
		instanceIndex < sourceInstanceWorldAabbs.Num();
		++instanceIndex)
	{
		const Math::AABB& instanceWorldAabb =
			sourceInstanceWorldAabbs[instanceIndex];
		if (!instanceWorldAabb.IsValid())
		{
			continue;
		}
		if (std::isfinite(result->m_lodPolicy.m_maxCameraDistance))
		{
			const glm::vec3 closestPoint = glm::clamp(
				cameraPosition,
				instanceWorldAabb.m_min,
				instanceWorldAabb.m_max);
			if (glm::distance(cameraPosition, closestPoint) >
				result->m_lodPolicy.m_maxCameraDistance)
			{
				continue;
			}
		}
		if (!frustum.OverlapsAABB(instanceWorldAabb))
		{
			continue;
		}

		const size_t firstMesh = instanceIndex * meshesPerInstance;
		const size_t endMesh = firstMesh + meshesPerInstance;
		bool bMeshesValid = endMesh <= result->m_meshes.Num();
		for (size_t meshIndex = firstMesh;
			bMeshesValid && meshIndex < endMesh;
			++meshIndex)
		{
			bMeshesValid = result->m_meshes[meshIndex].m_mesh.IsValid();
		}
		if (!bMeshesValid)
		{
			continue;
		}

		const float screenCoverage = result->m_lodPolicy.m_bEnabled ?
			CalculateScreenCoverage(instanceWorldAabb,
				viewMatrix,
				projectionMatrix) : 0.0f;
		for (size_t meshIndex = firstMesh;
			meshIndex < endMesh;
			++meshIndex)
		{
			RHIShadowMeshProxy shadowMesh =
				std::move(result->m_meshes[meshIndex]);
			shadowMesh.m_mesh = ResolveLodMesh(
				result->m_lodPolicy,
				screenCoverage,
				shadowMesh.m_mesh);
			result->m_meshes[writeMeshIndex] = std::move(shadowMesh);
			++writeMeshIndex;
		}
		visibleWorldAabb.Extend(instanceWorldAabb);
	}

	if (writeMeshIndex == 0u)
	{
		return {};
	}
	result->m_meshes.Resize(writeMeshIndex);
	result->m_instanceLodCulling = {};
	result->m_worldAabb = visibleWorldAabb;
	return result;
}

void RHISceneView::PrepareDebugDrawCommandLists(WorldPtr world)
{
	m_debugDraw.Reserve(m_cameras.Num());
	const DebugContext::DrawSnapshot debugDrawSnapshot = world->GetDebugContext()->GetDrawSnapshot();

	// TODO: Check the sync between CPUFrame and Recording
	for (const auto& camera : m_cameras)
	{
		auto task = Tasks::CreateTaskWithResult<RHI::RHICommandListPtr>("Record DebugContext Draw Command List",
			[=]()
			{
				const auto& matrix = camera.GetProjectionMatrix() * camera.GetViewMatrix();
				RHI::RHICommandListPtr secondaryCmdList = RHI::Renderer::GetDriver()->CreateCommandList(true, RHI::ECommandListQueue::Graphics);
				Sailor::RHI::Renderer::GetDriver()->SetDebugName(secondaryCmdList, "Draw Debug Mesh");
				auto commands = App::GetSubmodule<Renderer>()->GetDriverCommands();
				commands->BeginSecondaryCommandList(secondaryCmdList, false, true);
				world->GetDebugContext()->DrawDebugMesh(secondaryCmdList, matrix, debugDrawSnapshot);
				commands->EndCommandList(secondaryCmdList);

				return secondaryCmdList;
			}, EThreadType::RHI);

		task->Run();

		m_debugDraw.Emplace(std::move(task));
	}
}

void RHISceneView::Clear()
{
	m_rhiLightsData.Clear();
	m_boneMatrices.Clear();

	m_cameras.Clear();
	m_cameraTransforms.Clear();
	m_shadowMapsToUpdate.Clear();
	m_shadowMapsToBlit.Clear();
	m_shadowIndices.Clear();
	m_shadowAtlasTiles.Clear();

	m_drawImGui.Clear();
	m_debugDraw.Clear();
	m_snapshots.Clear();
	m_pathTracerProxies.Clear();
	m_pathTracerLights.Clear();
}

TVector<RHISceneViewProxy> RHISceneView::TraceScene(const Math::Frustum& frustum, bool bSkipMaterials) const
{
	const uint32_t NumProxiesPerTask = 1024;

	SAILOR_PROFILE_FUNCTION();

	TVector<RHISceneViewProxy> res;

	// Stationary
	TVector<RHIMeshProxy> meshProxies;
	m_stationaryOctree.Trace(frustum, meshProxies);
	TVector<Tasks::TaskPtr<TVector<RHISceneViewProxy>>> tasks;
	for (uint32_t i = 0; i < meshProxies.Num() / NumProxiesPerTask + 1; i++)
	{
		Tasks::TaskPtr<TVector<RHISceneViewProxy>> task = Tasks::CreateTaskWithResult<TVector<RHISceneViewProxy>>("Create list of scene view proxies",
			[i, &meshProxies, &m_world = m_world, bSkipMaterials]()
			{
				TVector<RHISceneViewProxy> temp;
				temp.Reserve(NumProxiesPerTask);
				for (uint32_t j = 0; j < NumProxiesPerTask; j++)
				{
					if (i * NumProxiesPerTask + j >= meshProxies.Num())
					{
						break;
					}

					auto& meshProxy = meshProxies[i * NumProxiesPerTask + j];
					auto& ecsData = m_world->GetECS<StaticMeshRendererECS>()->GetComponentData(meshProxy.m_staticMeshEcs);

					if (ecsData.GetMaterials().Num() == 0)
					{
						continue;
					}

					RHISceneViewProxy viewProxy;
					viewProxy.m_staticMeshEcs = meshProxy.m_staticMeshEcs;
					viewProxy.m_worldMatrix = meshProxy.m_worldMatrix;
					TVector<glm::mat4> modelMatrices;
					Math::AABB modelBounds;
					if (!ecsData.GetModel()->CollectRenderData(
							ecsData.GetMeshIndex(),
							viewProxy.m_meshes,
							modelMatrices,
							modelBounds))
					{
						continue;
					}
					viewProxy.m_meshModelMatrices.Reserve(
						modelMatrices.Num());
					for (const glm::mat4& modelMatrix : modelMatrices)
					{
						viewProxy.m_meshModelMatrices.Add(
							meshProxy.m_worldMatrix * modelMatrix);
					}
					viewProxy.m_skeletonOffset = ecsData.GetSkeletonOffset();
					viewProxy.m_frame = ecsData.GetFrameLastChange();
					viewProxy.m_bCastShadows = ecsData.ShouldCastShadow();
					viewProxy.m_worldAabb = modelBounds;
					viewProxy.m_worldAabb.Apply(viewProxy.m_worldMatrix);

					if (!bSkipMaterials)
					{
						viewProxy.m_overrideMaterials.Resize(viewProxy.m_meshes.Num());
						viewProxy.m_renderQueueTags.Reserve(viewProxy.m_meshes.Num());
#if defined(__APPLE__)
						viewProxy.m_materialTextureSamplers.Resize(viewProxy.m_meshes.Num());
						auto textureImporter = App::GetSubmodule<TextureImporter>();
#endif
						// TODO: Should we check AABB for each mesh in model?

						for (size_t i = 0; i < viewProxy.m_meshes.Num(); i++)
						{
							const size_t materialIndex =
								viewProxy.m_meshes[i]->ResolveMaterialIndex(
									i, ecsData.GetMaterials().Num());

							auto& material = ecsData.GetMaterials()[materialIndex];
							viewProxy.m_renderQueueTags.Add(material ? material->GetRenderState().GetTag() : 0u);
#if defined(__APPLE__)
							auto& requestedTextures = viewProxy.m_materialTextureSamplers[i];
							requestedTextures.Insert(0u);
#endif
							if (material && material->IsReady())
							{
								viewProxy.m_overrideMaterials[i] = material->GetOrAddRHI(viewProxy.m_meshes[i]->m_vertexDescription);
#if defined(__APPLE__)
								if (textureImporter)
								{
									for (const auto& sampler : material->GetSamplers())
									{
										const uint32_t textureIndex = sampler.m_second ? (uint32_t)textureImporter->GetTextureIndex(sampler.m_second->GetFileId()) : 0u;
										requestedTextures.Insert(textureIndex);
									}
								}
#endif
							}
						}
					}

					temp.Emplace(std::move(viewProxy));
				}

				return temp;
			});

		if (meshProxies.Num() < NumProxiesPerTask)
		{
			task->Execute();
			res.AddRange(std::move(task->m_result));

			break;
		}

		task->Run();
		tasks.Add(task);
	}

	for (auto& t : tasks)
	{
		t->Wait();
		res.AddRange(std::move(t->m_result));
	}

	// Static
	TVector<RHISceneViewProxy> proxies;
	m_staticOctree.Trace(frustum, proxies);

	res.AddRange(std::move(proxies));

	return res;
}

TVector<RHIShadowCasterProxyPtr> RHISceneView::TraceShadowCasters(const Math::Frustum& frustum) const
{
	SAILOR_PROFILE_FUNCTION();

	TVector<RHIShadowCasterProxyPtr> result;
	result.Reserve(m_stationaryOctree.Num() + m_staticOctree.Num());

	auto appendShadowCaster = [&result](const auto& proxy)
		{
			if (proxy.m_shadowCaster)
			{
				result.Add(proxy.m_shadowCaster);
			}
		};

	m_stationaryOctree.Trace(frustum, appendShadowCaster);
	m_staticOctree.Trace(frustum, appendShadowCaster);

	return result;
}

void RHISceneView::PrepareSnapshots()
{
	SAILOR_PROFILE_FUNCTION();

	for (uint32_t i = 0; i < m_cameras.Num(); i++)
	{
		auto& camera = m_cameras[i];
		RHISceneViewSnapshot res;

		Math::Frustum frustum;

		frustum.ExtractFrustumPlanes(m_cameraTransforms[i].Matrix(), camera.GetAspect(), camera.GetFov(), camera.GetZNear(), camera.GetZFar());

		res.m_deltaTime = m_deltaTime;
		res.m_frame = m_world->GetCurrentFrame();
		res.m_cameraIndex = i;
		res.m_cameraTransform = m_cameraTransforms[i];
		res.m_camera = TUniquePtr<CameraData>::Make();
		*res.m_camera = camera;
		res.m_pathTracerProxies = m_pathTracerProxies;
		res.m_pathTracerTLASInstances = m_pathTracerTLASInstances;
		res.m_pathTracerMaterials = m_pathTracerMaterials;
		res.m_pathTracerLights = m_pathTracerLights;

		res.m_totalNumLights = m_totalNumLights;
		res.m_rhiLightsData = m_rhiLightsData;
		res.m_boneMatrices = m_boneMatrices;
		res.m_drawImGui = m_drawImGui;
		res.m_shadowMapsToUpdate = std::move(m_shadowMapsToUpdate[i]);
		res.m_shadowMapsToBlit = std::move(m_shadowMapsToBlit[i]);
		res.m_shadowIndices = std::move(m_shadowIndices[i]);
		res.m_shadowAtlasTiles = std::move(m_shadowAtlasTiles[i]);
		res.m_proxies = TraceScene(frustum, false);
		auto* meshEcs = m_world ? m_world->GetECS<StaticMeshRendererECS>() : nullptr;
		const glm::vec3 cameraPosition = glm::vec3(m_cameraTransforms[i].m_position);
		const glm::mat4 viewMatrix = camera.GetViewMatrix();
		const glm::mat4 projectionMatrix = camera.GetProjectionMatrix();
		TVector<size_t> instanceLodCullingProxyIndices;
		TVector<uint8_t> instanceLodCullingProxyFlags(
			res.m_proxies.Num());
		instanceLodCullingProxyIndices.Reserve(res.m_proxies.Num());
		for (size_t proxyIndex = 0u;
			proxyIndex < res.m_proxies.Num();
			++proxyIndex)
		{
			if (SceneViewDetails::HasAlignedInstanceData(
				res.m_proxies[proxyIndex]))
			{
				instanceLodCullingProxyFlags[proxyIndex] = 1u;
				instanceLodCullingProxyIndices.Add(proxyIndex);
			}
		}

		const auto prepareInstanceLodCullingProxy =
			[&](size_t proxyIndex)
			{
				auto& taskProxy = res.m_proxies[proxyIndex];
				const auto decision = SceneViewDetails::ClassifyInstanceCell(
					taskProxy,
					frustum,
					cameraPosition,
					viewMatrix,
					projectionMatrix);
				if (SceneViewDetails::ApplyInstanceLodCullingDecision(
					taskProxy,
					decision,
					frustum,
					cameraPosition,
					viewMatrix,
					projectionMatrix))
				{
					taskProxy.m_shadowCaster = CreateLodShadowCaster(
						taskProxy.m_shadowCaster,
						m_world,
						camera,
						m_cameraTransforms[i]);
				}
			};

		TVector<Tasks::TaskPtr<void, void>> instanceLodCullingTasks;
		if (!instanceLodCullingProxyIndices.IsEmpty())
		{
			auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
			// Bound task allocation to the real worker pool. Strided ranges keep
			// expensive boundary chunks distributed when their costs differ.
			const size_t numTasks = (std::min)(
				instanceLodCullingProxyIndices.Num(),
				static_cast<size_t>((std::max)(
					scheduler ? scheduler->GetNumWorkerThreads() : 1u,
					1u)));
			instanceLodCullingTasks.Reserve(numTasks);
			for (size_t taskIndex = 0u; taskIndex < numTasks; ++taskIndex)
			{
				auto task = Tasks::CreateTask(
					"Prepare vegetation chunk LOD and culling",
					[&, taskIndex, numTasks]()
					{
						for (size_t proxyListIndex = taskIndex;
							proxyListIndex < instanceLodCullingProxyIndices.Num();
							proxyListIndex += numTasks)
						{
							prepareInstanceLodCullingProxy(
								instanceLodCullingProxyIndices[proxyListIndex]);
						}
					},
					EThreadType::Worker);
				task->Run();
				instanceLodCullingTasks.Add(std::move(task));
			}
		}

		for (size_t proxyIndex = 0u;
			proxyIndex < res.m_proxies.Num();
			++proxyIndex)
		{
			auto& proxy = res.m_proxies[proxyIndex];
			if (instanceLodCullingProxyFlags[proxyIndex] != 0u)
			{
				continue;
			}

			if (std::isfinite(proxy.m_lodPolicy.m_maxCameraDistance))
			{
				const glm::vec3 closest = glm::clamp(
					cameraPosition,
					proxy.m_worldAabb.m_min,
					proxy.m_worldAabb.m_max);
				if (glm::distance(cameraPosition, closest) >
					proxy.m_lodPolicy.m_maxCameraDistance)
				{
					ClearProxyMeshData(proxy);
					continue;
				}
			}

			if (proxy.m_lodPolicy.m_bEnabled)
			{
				ApplyCustomLodToMeshes(proxy.m_lodPolicy, proxy.m_worldAabb,
					camera, proxy.m_meshes);
			}
			else if (meshEcs && meshEcs->IsComponentRegistered(proxy.m_staticMeshEcs))
			{
				const auto& data = meshEcs->GetComponentData(proxy.m_staticMeshEcs);
				ApplyLodToMeshes(
					data,
					proxy.m_worldAabb,
					camera,
					proxy.m_meshes);
			}
			proxy.m_shadowCaster = CreateLodShadowCaster(
				proxy.m_shadowCaster,
				m_world,
				camera,
				m_cameraTransforms[i]);
		}
		for (auto& task : instanceLodCullingTasks)
		{
			task->Wait();
		}
		res.m_proxies.RemoveAll([](const RHISceneViewProxy& proxy)
			{
				return proxy.m_meshes.IsEmpty();
			});
		struct ShadowCasterWorkItem
		{
			size_t m_shadowMapIndex = 0u;
			size_t m_shadowCasterIndex = 0u;
		};
		TVector<Math::Frustum> shadowFrustums(
			res.m_shadowMapsToUpdate.Num());
		TVector<ShadowCasterWorkItem> shadowCasterWorkItems;
		for (size_t shadowMapIndex = 0u;
			shadowMapIndex < res.m_shadowMapsToUpdate.Num();
			++shadowMapIndex)
		{
			auto& shadowMap = res.m_shadowMapsToUpdate[shadowMapIndex];
			shadowFrustums[shadowMapIndex].ExtractFrustumPlanes(
				shadowMap.m_lightMatrix);
			for (size_t shadowCasterIndex = 0u;
				shadowCasterIndex < shadowMap.m_meshList.Num();
				++shadowCasterIndex)
			{
				auto& shadowCaster =
					shadowMap.m_meshList[shadowCasterIndex];
				if (!shadowCaster)
				{
					continue;
				}
				if (SceneViewDetails::HasAlignedInstanceData(*shadowCaster))
				{
					shadowCasterWorkItems.Add({
						shadowMapIndex,
						shadowCasterIndex });
					continue;
				}
				shadowCaster = CreateLodShadowCaster(
					shadowCaster,
					m_world,
					camera,
					m_cameraTransforms[i]);
			}
		}

		TVector<Tasks::TaskPtr<void, void>> shadowCasterTasks;
		if (!shadowCasterWorkItems.IsEmpty())
		{
			auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
			const size_t numTasks = (std::min)(
				shadowCasterWorkItems.Num(),
				static_cast<size_t>((std::max)(
					scheduler ? scheduler->GetNumWorkerThreads() : 1u,
					1u)));
			shadowCasterTasks.Reserve(numTasks);
			for (size_t taskIndex = 0u; taskIndex < numTasks; ++taskIndex)
			{
				auto task = Tasks::CreateTask(
					"Prepare vegetation shadow LOD and culling",
					[&, taskIndex, numTasks]()
					{
						for (size_t workItemIndex = taskIndex;
							workItemIndex < shadowCasterWorkItems.Num();
							workItemIndex += numTasks)
						{
							const ShadowCasterWorkItem& workItem =
								shadowCasterWorkItems[workItemIndex];
							auto& shadowCaster = res.m_shadowMapsToUpdate[
								workItem.m_shadowMapIndex].m_meshList[
								workItem.m_shadowCasterIndex];
							shadowCaster = CreateLodShadowCaster(
								shadowCaster,
								m_world,
								camera,
								m_cameraTransforms[i],
								&shadowFrustums[
									workItem.m_shadowMapIndex]);
						}
					},
					EThreadType::Worker);
				task->Run();
				shadowCasterTasks.Add(std::move(task));
			}
		}
		for (auto& task : shadowCasterTasks)
		{
			task->Wait();
		}
		for (auto& shadowMap : res.m_shadowMapsToUpdate)
		{
			shadowMap.m_meshList.RemoveAll(
				[](const RHIShadowCasterProxyPtr& shadowCaster)
				{
					return !shadowCaster || shadowCaster->m_meshes.IsEmpty();
				});
		}

		if (i < m_debugDraw.Num())
		{
			res.m_debugDrawSecondaryCmdList = m_debugDraw[i];
		}
		m_snapshots.Emplace(std::move(res));
	}
}

const TVector<RHIMaterialPtr>& RHISceneViewProxy::GetMaterials() const
{
	// TODO: Create default materials inside model
	return m_overrideMaterials;
}
