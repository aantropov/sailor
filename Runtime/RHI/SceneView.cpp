#include "SceneView.h"
#include "ECS/CameraECS.h"
#include "ECS/TransformECS.h"
#include "ECS/StaticMeshRendererECS.h"
#include "Engine/GameObject.h"
#include "Math/Transform.h"
#include "Engine/World.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "RHI/DebugContext.h"
#include "RHI/CommandList.h"

using namespace Sailor;
using namespace Sailor::RHI;

void RHISceneView::PrepareDebugDrawCommandLists(WorldPtr world)
{
	m_debugDraw.Reserve(m_cameras.Num());

	// TODO: Check the sync between CPUFrame and Recording
	for (const auto& camera : m_cameras)
	{
		auto task = Tasks::Scheduler::CreateTaskWithResult<RHI::RHICommandListPtr>("Record DebugContext Draw Command List",
			[=]()
			{
				const auto& matrix = camera.GetProjectionMatrix() * camera.GetViewMatrix();
				RHI::RHICommandListPtr secondaryCmdList = RHI::Renderer::GetDriver()->CreateCommandList(true, false);
				Sailor::RHI::Renderer::GetDriver()->SetDebugName(secondaryCmdList, "Draw Debug Mesh");
				auto commands = App::GetSubmodule<Renderer>()->GetDriverCommands();
				commands->BeginSecondaryCommandList(secondaryCmdList, false, true);
				world->GetDebugContext()->DrawDebugMesh(secondaryCmdList, matrix);
				commands->EndCommandList(secondaryCmdList);

				return secondaryCmdList;
			}, Tasks::EThreadType::RHI);

		task->Run();

		m_debugDraw.Emplace(std::move(task));
	}
}

void RHISceneView::Clear()
{
	m_rhiLightsData.Clear();
	m_directionalLights.Clear();

	m_sortedPointLights.Clear();
	m_sortedSpotLights.Clear();

	m_cameras.Clear();
	m_cameraTransforms.Clear();

	m_drawImGui.Clear();
	m_debugDraw.Clear();
	m_snapshots.Clear();
}

TVector<RHISceneViewProxy> RHISceneView::TraceScene(const Math::Frustum& frustum) const
{
	TVector<RHISceneViewProxy> res;

	// Stationary
	TVector<RHIMeshProxy> meshProxies;
	m_stationaryOctree.Trace(frustum, meshProxies);

	res.Reserve(meshProxies.Num());
	for (auto& meshProxy : meshProxies)
	{
		auto& ecsData = m_world->GetECS<StaticMeshRendererECS>()->GetComponentData(meshProxy.m_staticMeshEcs);

		if (ecsData.GetMaterials().Num() == 0)
		{
			continue;
		}

		RHISceneViewProxy viewProxy;
		viewProxy.m_staticMeshEcs = meshProxy.m_staticMeshEcs;
		viewProxy.m_worldMatrix = meshProxy.m_worldMatrix;
		viewProxy.m_meshes = ecsData.GetModel()->GetMeshes();
		viewProxy.m_overrideMaterials.Clear();
		
		viewProxy.m_worldAabb = ecsData.GetModel()->GetBoundsAABB();
		viewProxy.m_worldAabb.Apply(viewProxy.m_worldMatrix);

		for (size_t i = 0; i < viewProxy.m_meshes.Num(); i++)
		{
			size_t materialIndex = (std::min)(i, ecsData.GetMaterials().Num() - 1);

			auto& material = ecsData.GetMaterials()[materialIndex];

			if (material && material->IsReady())
			{
				viewProxy.m_overrideMaterials.Add(material->GetOrAddRHI(viewProxy.m_meshes[i]->m_vertexDescription));
			}
		}

		res.Emplace(std::move(viewProxy));
	}

	// Static
	TVector<RHISceneViewProxy> proxies;
	m_staticOctree.Trace(frustum, proxies);
	res.Reserve(meshProxies.Num() + proxies.Num());

	for (auto& proxy : proxies)
	{
		res.Emplace(std::move(proxy));
	}

	return res;
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
		res.m_cameraTransform = m_cameraTransforms[i];
		res.m_camera = TUniquePtr<CameraData>::Make();
		*res.m_camera = camera;

		res.m_totalNumLights = m_totalNumLights;
		res.m_rhiLightsData = m_rhiLightsData;
		res.m_drawImGui = m_drawImGui;
		res.m_sortedSpotLights = std::move(m_sortedSpotLights[i]);
		res.m_sortedPointLights = std::move(m_sortedPointLights[i]);
		res.m_directionalLights = m_directionalLights;

		res.m_csmMeshLists = std::move(m_csmMeshLists[i]);
		
		res.m_proxies = std::move(TraceScene(frustum));

		res.m_debugDrawSecondaryCmdList = m_debugDraw[i];
		m_snapshots.Emplace(std::move(res));
	}
}

const TVector<RHIMaterialPtr>& RHISceneViewProxy::GetMaterials() const
{
	// TODO: Create default materials inside model
	return m_overrideMaterials.Num() > 0 ? m_overrideMaterials : m_overrideMaterials;
}
