#include "Components/CameraComponent.h"
#include "Engine/GameObject.h"
#include "ECS/CameraECS.h"
#include "Math/Math.h"

using namespace Sailor;
using namespace Sailor::JobSystem;

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

	GetData().SetOwner(GetOwner());

	m_aspect = CalculateAspect();
	m_fovDegrees = 90.0f;
	m_zNear = 0.01f;
	m_zFar = 5000.0f;

	GetData().SetProjectionMatrix(Math::PerspectiveRH(glm::radians(m_fovDegrees), m_aspect, m_zNear, m_zFar));
}

CameraData& CameraComponent::GetData()
{
	auto ecs = GetOwner()->GetWorld()->GetECS<CameraECS>();
	return ecs->GetComponentData(m_handle);
}

void CameraComponent::Tick(float deltaTime)
{
	const float aspect = CalculateAspect();

	if (m_aspect != aspect)
	{
		m_aspect = aspect;
		GetData().SetProjectionMatrix(Math::PerspectiveRH(glm::radians(90.0f), aspect, m_zNear, m_zFar));
	}
}

void CameraComponent::EndPlay()
{
	GetOwner()->GetWorld()->GetECS<CameraECS>()->UnregisterComponent(m_handle);
}
