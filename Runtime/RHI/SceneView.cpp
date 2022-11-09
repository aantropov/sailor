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

using namespace Sailor;
using namespace Sailor::RHI;

void RHISceneView::PrepareDebugDrawCommandLists(WorldPtr world)
{
	m_debugDraw.Reserve(m_cameras.Num());

	for (const auto& camera : m_cameras)
	{
		const auto& matrix = camera.GetProjectionMatrix() * camera.GetViewMatrix();
		RHI::RHICommandListPtr secondaryCmdList = RHI::Renderer::GetDriver()->CreateCommandList(true, false);
		auto commands = App::GetSubmodule<Renderer>()->GetDriverCommands();
		commands->BeginSecondaryCommandList(secondaryCmdList, false, true);
		world->GetDebugContext()->DrawDebugMesh(secondaryCmdList, matrix);
		commands->EndCommandList(secondaryCmdList);

		m_debugDraw.Emplace(std::move(secondaryCmdList));
	}
}

void RHISceneView::PrepareSnapshots()
{
	SAILOR_PROFILE_FUNCTION();

	for (uint32_t i = 0; i < m_cameras.Num(); i++)
	{
		auto& camera = m_cameras[i];
		RHISceneViewSnapshot res;

		Math::Frustum frustum;

		frustum.ExtractFrustumPlanes(m_cameraTransforms[i], camera.GetAspect(), camera.GetFov(), camera.GetZNear(), camera.GetZFar());

		res.m_cameraPosition = m_cameraTransforms[i].m_position;
		res.m_camera = TUniquePtr<CameraData>::Make();
		*res.m_camera = camera;

		res.m_numLights = m_numLights;
		res.m_rhiLightsData = m_rhiLightsData;

		// Stationary
		TVector<RHIMeshProxy> meshProxies;
		m_stationaryOctree.Trace(frustum, meshProxies);

		res.m_proxies.Reserve(meshProxies.Num());
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
			for (size_t i = 0; i < viewProxy.m_meshes.Num(); i++)
			{
				size_t materialIndex = (std::min)(i, ecsData.GetMaterials().Num() - 1);

				auto& material = ecsData.GetMaterials()[materialIndex];

				if (material && material->IsReady())
				{
					viewProxy.m_overrideMaterials.Add(material->GetOrAddRHI(viewProxy.m_meshes[i]->m_vertexDescription));
				}
			}

			res.m_proxies.Emplace(std::move(viewProxy));
		}

		// Static
		TVector<RHISceneViewProxy> proxies;
		m_staticOctree.Trace(frustum, proxies);
		res.m_proxies.Reserve(meshProxies.Num() + proxies.Num());

		for (auto& proxy : proxies)
		{
			res.m_proxies.Emplace(std::move(proxy));
		}

		res.m_debugDrawSecondaryCmdList = m_debugDraw[i];
		m_snapshots.Emplace(std::move(res));
	}
}

const TVector<RHIMaterialPtr>& RHISceneViewProxy::GetMaterials() const
{
	// TODO: Create default materials inside model
	return m_overrideMaterials.Num() > 0 ? m_overrideMaterials : m_overrideMaterials;
}
