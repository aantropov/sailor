#include "Components/MeshRendererComponent.h"
#include "Engine/GameObject.h"
#include "ECS/StaticMeshRendererECS.h"
#include "AssetRegistry/Material/MaterialImporter.h"

#include <algorithm>
#include <cmath>
#include <functional>

using namespace Sailor;
using namespace Sailor::Tasks;

void MeshRendererComponent::Initialize()
{
	auto ecs = GetOwner()->GetWorld()->GetECS<StaticMeshRendererECS>();
	m_handle = ecs->RegisterComponent();

	GetData().SetOwner(GetOwner());
	GetData().SetMeshIndex(m_meshIndex);
	GetData().SetLodSettings(
		m_minLod,
		m_maxLod,
		m_screenCoverageThresholds);
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
	m_handle = ECS::InvalidIndex;
}

void MeshRendererComponent::SetModel(const ModelPtr& model)
{
	GetData().SetModel(model);
	RebuildMaterials();
}

void MeshRendererComponent::SetMeshIndex(int32_t meshIndex)
{
	m_meshIndex = meshIndex;
	if (m_handle != ECS::InvalidIndex)
	{
		GetData().SetMeshIndex(meshIndex);
	}
}

void MeshRendererComponent::SetOverrideMaterials(const TVector<FileId>& overrideMaterials)
{
	m_overrideMaterials = overrideMaterials;
	RebuildMaterials();
}

void MeshRendererComponent::SetMinLod(uint32_t minLod)
{
	m_minLod = minLod;
	m_maxLod = (std::max)(m_maxLod, m_minLod);
	if (m_handle != ECS::InvalidIndex)
	{
		GetData().SetLodSettings(
			m_minLod,
			m_maxLod,
			m_screenCoverageThresholds);
	}
}

void MeshRendererComponent::SetMaxLod(uint32_t maxLod)
{
	m_maxLod = (std::max)(maxLod, m_minLod);
	if (m_handle != ECS::InvalidIndex)
	{
		GetData().SetLodSettings(
			m_minLod,
			m_maxLod,
			m_screenCoverageThresholds);
	}
}

void MeshRendererComponent::SetScreenCoverageThresholds(
	const TVector<float>& screenCoverageThresholds)
{
	m_screenCoverageThresholds = screenCoverageThresholds;
	for (float& threshold : m_screenCoverageThresholds)
	{
		threshold = std::isfinite(threshold) ?
			(std::clamp)(threshold, 0.0f, 100.0f) : 0.0f;
	}
	std::sort(
		m_screenCoverageThresholds.begin(),
		m_screenCoverageThresholds.end(),
		std::greater<float>());
	if (m_handle != ECS::InvalidIndex)
	{
		GetData().SetLodSettings(
			m_minLod,
			m_maxLod,
			m_screenCoverageThresholds);
	}
}

void MeshRendererComponent::RebuildMaterials()
{
	auto& materials = GetMaterials();
	materials.Clear();

	const ModelPtr& model = GetModel();
	if (!model || !model->GetFileId())
	{
		GetData().MarkDirty();
		return;
	}

	if (auto* modelImporter = App::GetSubmodule<ModelImporter>())
	{
		modelImporter->LoadDefaultMaterials(model->GetFileId(), materials);
	}

	if (auto* materialImporter = App::GetSubmodule<MaterialImporter>())
	{
		for (size_t materialIndex = 0; materialIndex < m_overrideMaterials.Num(); ++materialIndex)
		{
			const FileId& materialFileId = m_overrideMaterials[materialIndex];
			if (!materialFileId)
			{
				continue;
			}

			MaterialPtr overrideMaterial;
			if (!materialImporter->LoadMaterial(materialFileId, overrideMaterial) || !overrideMaterial)
			{
				continue;
			}

			if (materialIndex >= materials.Num())
			{
				const size_t firstNewSlot = materials.Num();
				const MaterialPtr fallbackMaterial = materials.IsEmpty()
					? overrideMaterial
					: *materials.Last();
				materials.Resize(materialIndex + 1);
				for (size_t slot = firstNewSlot; slot <= materialIndex; ++slot)
				{
					materials[slot] = fallbackMaterial;
				}
			}

			materials[materialIndex] = overrideMaterial;
		}
	}

	GetData().MarkDirty();
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
