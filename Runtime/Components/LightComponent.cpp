#include "Components/LightComponent.h"
#include "Engine/GameObject.h"
#include "ECS/TransformECS.h"
#include "ECS/LightingECS.h"
#include "Math/Math.h"

#include <cmath>

using namespace Sailor;
using namespace Sailor::Tasks;

void LightComponent::Initialize()
{
	auto ecs = GetOwner()->GetWorld()->GetECS<LightingECS>();
	m_handle = ecs->RegisterComponent();

	auto& ecsData = GetData();

	ecsData.SetOwner(GetOwner());
	ecsData.MarkDirty();
}

void LightComponent::BeginPlay()
{
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
	m_handle = ECS::InvalidIndex;
}

void LightComponent::OnGizmo()
{
	const glm::vec4 worldPosition = GetOwner()->GetTransformComponent().GetWorldPosition();
	const glm::vec3 forward = GetOwner()->GetTransformComponent().GetForwardVector();

	const float originSize = glm::clamp(glm::abs(GetData().m_radius), 25.0f, 250.0f);

	GetOwner()->GetWorld()->GetDebugContext()->DrawOrigin(worldPosition, GetOwner()->GetTransformComponent().GetCachedWorldMatrix(), originSize);
	GetOwner()->GetWorld()->GetDebugContext()->DrawArrow(worldPosition, glm::vec3(worldPosition) + forward * originSize, vec4(1, 1, 1, 1));
}

void LightComponent::SetCutOff(const glm::vec2& innerOuterDegrees)
{
	LightData& lightData = GetData();

	if (innerOuterDegrees != lightData.m_cutOff)
	{
		lightData.m_cutOff = innerOuterDegrees;
		lightData.MarkDirty();
	}
}

void LightComponent::SetIntensity(const glm::vec3& value)
{
	LightData& lightData = GetData();

	if (value != lightData.m_intensity)
	{
		lightData.m_intensity = value;
		lightData.MarkDirty();
	}
}

void LightComponent::SetIndirectLightingIntensity(float value)
{
	LightData& lightData = GetData();
	value = std::isfinite(value) ? (std::max)(value, 0.0f) : 0.0f;

	if (value != lightData.m_indirectLightingIntensity)
	{
		lightData.m_indirectLightingIntensity = value;
		lightData.MarkDirty();
	}
}

void LightComponent::SetGlobalIlluminationMode(
	ELightGlobalIlluminationMode value)
{
	LightData& lightData = GetData();

	if (value != lightData.m_globalIlluminationMode)
	{
		lightData.m_globalIlluminationMode = value;
		lightData.MarkDirty();
	}
}

void LightComponent::SetAttenuation(const glm::vec3& value)
{
	LightData& lightData = GetData();

	if (value != lightData.m_attenuation)
	{
		lightData.m_attenuation = value;
		lightData.MarkDirty();
	}
}

void LightComponent::SetRadius(float value)
{
	LightData& lightData = GetData();
	value = (std::max)(value, 0.01f);

	if (value != lightData.m_radius)
	{
		lightData.m_radius = value;
		lightData.MarkDirty();
	}
}

void LightComponent::SetLightType(ELightType value)
{
	LightData& lightData = GetData();

	if (value != lightData.m_type)
	{
		lightData.m_type = value;
		lightData.MarkDirty();
	}
}

void LightComponent::SetShadowType(RHI::EShadowType value)
{
	LightData& lightData = GetData();

	if (value != lightData.m_shadowType)
	{
		lightData.m_shadowType = value;
		lightData.MarkDirty();
	}
}

void LightComponent::SetShadowQuality(ELightShadowQuality value)
{
	LightData& lightData = GetData();

	if (value != lightData.m_shadowQuality)
	{
		lightData.m_shadowQuality = value;
		lightData.MarkDirty();
	}
}

void LightComponent::SetShadowFilter(ELightShadowFilter value)
{
	LightData& lightData = GetData();

	if (value != lightData.m_shadowFilter)
	{
		lightData.m_shadowFilter = value;
		lightData.MarkDirty();
	}
}
