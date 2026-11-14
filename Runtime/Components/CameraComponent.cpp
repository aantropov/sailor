#include "Components/CameraComponent.h"
#include "Engine/GameObject.h"
#include "ECS/CameraECS.h"
#include "Math/Math.h"

using namespace Sailor;
using namespace Sailor::Tasks;

float CameraComponent::CalculateAspect()
{
	const int32_t width = Sailor::App::GetMainWindow()->GetRenderArea().x;
	const int32_t height = Sailor::App::GetMainWindow()->GetRenderArea().y;

	if (height <= 0)
	{
		return 1.0f;
	}

	return width / (float)height;
}

void CameraComponent::SetFov(float value)
{
	GetData().SetFov(value);
	GetData().SetProjectionMatrix(Math::PerspectiveRH(glm::radians(GetFov()), GetAspect(), GetZNear(), GetZFar()));
}

void CameraComponent::SetAspect(float value)
{
	GetData().SetAspect(value);
	GetData().SetProjectionMatrix(Math::PerspectiveRH(glm::radians(GetFov()), GetAspect(), GetZNear(), GetZFar()));
}

void CameraComponent::SetZNear(float value)
{
	GetData().SetZNear(value);
	GetData().SetProjectionMatrix(Math::PerspectiveRH(glm::radians(GetFov()), GetAspect(), GetZNear(), GetZFar()));
}

void CameraComponent::SetZFar(float value)
{
	GetData().SetZFar(value);
	GetData().SetProjectionMatrix(Math::PerspectiveRH(glm::radians(GetFov()), GetAspect(), GetZNear(), GetZFar()));
}

void CameraComponent::SetRendererConfig(Sailor::FileId value)
{
	m_rendererConfig = value;
	if (m_handle != ECS::InvalidIndex)
	{
		GetData().SetRendererConfig(value);
	}
}

void CameraComponent::SetRenderTarget(TexturePtr value)
{
	m_renderTarget = value;
	if (m_handle != ECS::InvalidIndex)
	{
		GetData().SetRenderTarget(value);
	}
}

void CameraComponent::SetQualityPreset(Settings::EGraphicsQualitySelection value)
{
	m_qualityPreset = value;
	if (m_handle != ECS::InvalidIndex)
	{
		GetData().SetQualityPreset(value);
	}
}

void CameraComponent::ResetMotionHistory()
{
	GetData().ResetMotionHistory();
}

void CameraComponent::Initialize()
{
	auto ecs = GetOwner()->GetWorld()->GetECS<CameraECS>();
	m_handle = ecs->RegisterComponent();

	auto& ecsData = GetData();

	ecsData.SetOwner(GetOwner());
	ecsData.SetInstanceId(GetInstanceId());
	ecsData.SetRendererConfig(m_rendererConfig);
	ecsData.SetRenderTarget(m_renderTarget);
	ecsData.SetQualityPreset(m_qualityPreset);

	ecsData.SetAspect(CalculateAspect());
	ecsData.SetFov(90.0f);
	ecsData.SetZNear(0.1f);
	ecsData.SetZFar(50000.0f);

	GetData().SetProjectionMatrix(Math::PerspectiveRH(glm::radians(GetFov()), GetAspect(), GetZNear(), GetZFar()));
}

void CameraComponent::BeginPlay()
{
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
	UpdateProjectionMatrix();
}

void CameraComponent::EditorTick(float deltaTime)
{
	UpdateProjectionMatrix();
}

void CameraComponent::UpdateProjectionMatrix()
{
	float aspect = CalculateAspect();

	if (m_renderTarget && m_renderTarget->IsReady())
	{
		const int32_t width = m_renderTarget->GetWidth();
		const int32_t height = m_renderTarget->GetHeight();
		if (width > 0 && height > 0)
		{
			aspect = (float)width / (float)height;
		}
	}

	if (GetAspect() != aspect)
	{
		GetData().SetAspect(aspect);
		GetData().SetProjectionMatrix(Math::PerspectiveRH(glm::radians(GetFov()), aspect, GetZNear(), GetZFar()));
	}
}

void CameraComponent::EndPlay()
{
	GetOwner()->GetWorld()->GetECS<CameraECS>()->UnregisterComponent(m_handle);
	m_handle = ECS::InvalidIndex;
}
