#include "ECS/StaticMeshRendererECS.h"
#include "Engine/GameObject.h"

using namespace Sailor;
using namespace Sailor::JobSystem;

void StaticMeshRendererData::SetModel(const ModelPtr& model)
{
	m_model = model;

	m_materials.Clear();
	m_materials.Reserve(model->GetDefaultMaterials().Num());

	for (auto& el : model->GetDefaultMaterials())
	{
		m_materials.Add(el);
	}
}

JobSystem::ITaskPtr StaticMeshRendererECS::Tick(float deltaTime)
{
	return nullptr;
}
