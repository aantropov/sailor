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
	m_sceneViewProxiesCache->m_octree.Clear();

	for (auto& data : m_components)
	{
		if (data.m_bIsActive && data.GetModel() && data.GetModel()->IsReady())
		{
			const auto& ownerTransform = data.m_owner.StaticCast<GameObject>()->GetTransformComponent();

			RHI::RHISceneViewProxy proxy;
			proxy.m_staticMeshEcs = GetComponentIndex(&data);
			proxy.m_worldMatrix = ownerTransform.GetCachedWorldMatrix();
			proxy.m_meshes = data.GetModel()->GetMeshes();

			proxy.m_overrideMaterials.Clear();
			for (size_t i = 0; i < proxy.m_meshes.Num(); i++)
			{
				size_t materialIndex = (std::min)(i, data.GetMaterials().Num() - 1);
				proxy.m_overrideMaterials.Add(data.GetMaterials()[materialIndex]->GetOrAddRHI(proxy.m_meshes[i]->m_vertexDescription));
			}

			const auto& bounds = data.GetModel()->GetBoundsAABB();
			const auto& center = proxy.m_worldMatrix * glm::vec4(bounds.GetCenter(), 1);
			m_sceneViewProxiesCache->m_octree.Insert(center, bounds.GetExtents(), proxy);
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
