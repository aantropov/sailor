#include "Components/LightComponent.h"
#include "Engine/GameObject.h"
#include "ECS/TransformECS.h"
#include "ECS/LightingECS.h"
#include "Math/Math.h"

using namespace Sailor;
using namespace Sailor::Tasks;

void LightComponent::BeginPlay()
{
	auto ecs = GetOwner()->GetWorld()->GetECS<LightingECS>();
	m_handle = ecs->RegisterComponent();

	auto& ecsData = GetData();

	ecsData.SetOwner(GetOwner());
}

LightData& LightComponent::GetData()
{
	auto ecs = GetOwner()->GetWorld()->GetECS<LightingECS>();
	return ecs->GetComponentData(m_handle);
}

const LightData& LightComponent::GetData() const
{
	auto ecs = GetOwner()->GetWorld()->GetECS<LightingECS>();
	return ecs->GetComponentData(m_handle);
}

void LightComponent::EndPlay()
{
	GetOwner()->GetWorld()->GetECS<LightingECS>()->UnregisterComponent(m_handle);
}

void LightComponent::OnGizmo()
{
	const glm::vec4 worldPosition = GetOwner()->GetTransformComponent().GetWorldPosition();
	GetOwner()->GetWorld()->GetDebugContext()->DrawOrigin(worldPosition, 30.0f);
}