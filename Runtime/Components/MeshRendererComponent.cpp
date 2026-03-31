#include "Components/MeshRendererComponent.h"
#include "Engine/GameObject.h"
#include "ECS/StaticMeshRendererECS.h"

using namespace Sailor;
using namespace Sailor::Tasks;

void MeshRendererComponent::Initialize()
{
	auto ecs = GetOwner()->GetWorld()->GetECS<StaticMeshRendererECS>();
	m_handle = ecs->RegisterComponent();

	GetData().SetOwner(GetOwner());
}

void MeshRendererComponent::BeginPlay()
{
	//if (auto modelFileId = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<ModelAssetInfoPtr>("Models/KnightArtorias/Artorias.fbx"))
}

StaticMeshRendererData& MeshRendererComponent::GetData()
{
	auto ecs = GetOwner()->GetWorld()->GetECS<StaticMeshRendererECS>();
	return ecs->GetComponentData(m_handle);
}

const StaticMeshRendererData& MeshRendererComponent::GetData() const
{
	auto ecs = GetOwner()->GetWorld()->GetECS<StaticMeshRendererECS>();
	return ecs->GetComponentData(m_handle);
}

void MeshRendererComponent::EndPlay()
{
	GetOwner()->GetWorld()->GetECS<StaticMeshRendererECS>()->UnregisterComponent(m_handle);
}

void MeshRendererComponent::SetModel(const ModelPtr& model)
{
	GetData().SetModel(model);
	GetData().MarkDirty();

	if (model && model->GetFileId())
	{
		App::GetSubmodule<ModelImporter>()->LoadDefaultMaterials(model->GetFileId(), GetMaterials());
	}
}

bool MeshRendererComponent::LoadModel(const std::string& path)
{
	auto assetRegistry = App::GetSubmodule<AssetRegistry>();
	auto modelImporter = App::GetSubmodule<ModelImporter>();
	if (!assetRegistry || !modelImporter)
	{
		return false;
	}

	const FileId modelId = assetRegistry->GetOrLoadFile(path);
	if (!modelId)
	{
		return false;
	}

	ModelPtr model;
	if (!modelImporter->LoadModel_Immediate(modelId, model) || !model)
	{
		return false;
	}

	SetModel(model);
	return true;
}