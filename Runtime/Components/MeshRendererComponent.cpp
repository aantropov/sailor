#include "Components/MeshRendererComponent.h"
#include "Engine/GameObject.h"
#include "ECS/StaticMeshRendererECS.h"

using namespace Sailor;
using namespace Sailor::JobSystem;

void MeshRendererComponent::BeginPlay()
{
	auto ecs = GetOwner()->GetWorld()->GetECS<StaticMeshRendererECS>();
	m_handle = ecs->RegisterComponent();

	GetData().SetOwner(GetOwner());

	if (auto modelUID = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<ModelAssetInfoPtr>("Models/Sponza/sponza.obj"))
	{
		App::GetSubmodule<ModelImporter>()->LoadModel(modelUID->GetUID(), GetModel());
		App::GetSubmodule<ModelImporter>()->LoadDefaultMaterials(modelUID->GetUID(), GetMaterials());
	}
}

StaticMeshRendererData& MeshRendererComponent::GetData()
{
	auto ecs = GetOwner()->GetWorld()->GetECS<StaticMeshRendererECS>();
	return ecs->GetComponentData(m_handle);
}

void MeshRendererComponent::EndPlay()
{
	GetOwner()->GetWorld()->GetECS<StaticMeshRendererECS>()->UnregisterComponent(m_handle);
}
