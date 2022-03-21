#include "Components/CameraComponent.h"
#include "Engine/GameObject.h"
#include "ECS/CameraECS.h"

using namespace Sailor;
using namespace Sailor::JobSystem;

void CameraComponent::BeginPlay()
{
	auto ecs = GetOwner()->GetWorld()->GetECS<CameraECS>();
	m_handle = ecs->RegisterComponent();

	GetData().SetOwner(GetOwner());

	auto width = Sailor::App::GetViewportWindow()->GetWidth();
	auto height = Sailor::App::GetViewportWindow()->GetHeight();
	
	float aspect = (height + width) > 0 ? width / (float)height : 1.0f;
	
	glm::mat4 projection = glm::perspective(glm::radians(90.0f), aspect, 0.1f, 10000.0f);
	projection[1][1] *= -1;

	GetData().SetProjectionMatrix(projection);
}

CameraData& CameraComponent::GetData()
{
	auto ecs = GetOwner()->GetWorld()->GetECS<CameraECS>();
	return ecs->GetComponentData(m_handle);
}

void CameraComponent::EndPlay()
{
	GetOwner()->GetWorld()->GetECS<CameraECS>()->UnregisterComponent(m_handle);
}
