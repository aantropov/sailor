#include "ECS/StaticMeshRendererECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "RHI/Material.h"
#include "RHI/Fence.h"

using namespace Sailor;
using namespace Sailor::Tasks;

void StaticMeshRendererECS::BeginPlay()
{
	m_sceneViewProxiesCache = RHI::RHISceneViewPtr::Make();
	m_perInstanceData = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();

	// Do we need support UBO & SSBO simulteniously?
	//bool bNeedsStorageBuffer = m_testMaterial->GetBindings()->NeedsStorageBuffer() ? EShaderBindingType::StorageBuffer : EShaderBindingType::UniformBuffer;
	Sailor::RHI::Renderer::GetDriver()->AddSsboToShaderBindings(m_perInstanceData, "data", sizeof(glm::mat4x4), 7, 0);
}

Tasks::ITaskPtr StaticMeshRendererECS::Tick(float deltaTime)
{
	//TODO: Resolve New/Delete components
	for (auto& data : m_components)
	{
		if (data.m_bIsActive && data.GetModel() && data.GetModel()->IsReady())
		{
			const auto& ownerTransform = data.m_owner.StaticCast<GameObject>()->GetTransformComponent();
			if (ownerTransform.GetLastFrameChanged() > data.m_lastChanges)
			{
				RHI::RHIMeshProxy proxy;
				proxy.m_staticMeshEcs = GetComponentIndex(&data);
				proxy.m_worldMatrix = ownerTransform.GetCachedWorldMatrix();

				const auto& bounds = data.GetModel()->GetBoundsAABB();
				const auto& center = proxy.m_worldMatrix * glm::vec4(bounds.GetCenter(), 1);

				// TODO: world matrix should impact on bounds
				m_sceneViewProxiesCache->m_octree.Update(center, bounds.GetExtents(), proxy);

				data.m_lastChanges = ownerTransform.GetLastFrameChanged();
			}
		}
	}

	return nullptr;
}

void StaticMeshRendererECS::CopySceneView(RHI::RHISceneViewPtr& outProxies)
{
	outProxies->m_octree = m_sceneViewProxiesCache->m_octree;
}

void StaticMeshRendererECS::EndPlay()
{
	m_perInstanceData.Clear();
	m_sceneViewProxiesCache.Clear();
}
