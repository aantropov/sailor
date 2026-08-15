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

namespace
{
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

	RHIShadowCasterProxyPtr CreateLodShadowCaster(
		const RHIShadowCasterProxyPtr& source,
		WorldPtr world,
		const CameraData& camera)
	{
		if (!source || !world)
		{
			return source;
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
		res.m_proxies = TraceScene(frustum, false);
		auto* meshEcs = m_world ? m_world->GetECS<StaticMeshRendererECS>() : nullptr;
		const glm::vec3 cameraPosition = glm::vec3(m_cameraTransforms[i].m_position);
		res.m_proxies.RemoveAll([&](const RHISceneViewProxy& proxy)
			{
				if (!std::isfinite(proxy.m_lodPolicy.m_maxCameraDistance))
				{
					return false;
				}
				const glm::vec3 closest = glm::clamp(
					cameraPosition, proxy.m_worldAabb.m_min, proxy.m_worldAabb.m_max);
				return glm::distance(cameraPosition, closest) > proxy.m_lodPolicy.m_maxCameraDistance;
			});
		for (auto& proxy : res.m_proxies)
		{
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
				proxy.m_shadowCaster, m_world, camera);
		}
		for (auto& shadowMap : res.m_shadowMapsToUpdate)
		{
			for (auto& shadowCaster : shadowMap.m_meshList)
			{
				shadowCaster = CreateLodShadowCaster(
					shadowCaster,
					m_world,
					camera);
			}
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
