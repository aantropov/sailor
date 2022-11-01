#include "Components/CameraComponent.h"
#include "Engine/GameObject.h"
#include "ECS/CameraECS.h"
#include "Math/Math.h"

using namespace Sailor;
using namespace Sailor::Tasks;

float CameraComponent::CalculateAspect()
{
	const int32_t width = Sailor::App::GetViewportWindow()->GetWidth();
	const int32_t height = Sailor::App::GetViewportWindow()->GetHeight();

	return (height + width) > 0 ? width / (float)height : 1.0f;
}

void CameraComponent::BeginPlay()
{
	auto ecs = GetOwner()->GetWorld()->GetECS<CameraECS>();
	m_handle = ecs->RegisterComponent();

	auto& ecsData = GetData();

	ecsData.SetOwner(GetOwner());

	ecsData.SetAspect(CalculateAspect());
	ecsData.SetFov(90.0f);
	ecsData.SetZNear(0.1f);
	ecsData.SetZFar(50000.0f);

	GetData().SetProjectionMatrix(Math::PerspectiveRH(glm::radians(GetFov()), GetAspect(), GetZNear(), GetZFar()));
}

CameraData& CameraComponent::GetData()
{
	auto ecs = GetOwner()->GetWorld()->GetECS<CameraECS>();
	return ecs->GetComponentData(m_handle);
}

const CameraData& CameraComponent::GetData() const
{
	auto ecs = GetOwner()->GetWorld()->GetECS<CameraECS>();
	return ecs->GetComponentData(m_handle);
}

void CameraComponent::Tick(float deltaTime)
{
	const float aspect = CalculateAspect();

	if (GetAspect() != aspect)
	{
		GetData().SetAspect(aspect);
		GetData().SetProjectionMatrix(Math::PerspectiveRH(glm::radians(90.0f), aspect, GetZNear(), GetZFar()));
	}
}

void CameraComponent::EndPlay()
{
	GetOwner()->GetWorld()->GetECS<CameraECS>()->UnregisterComponent(m_handle);
}
