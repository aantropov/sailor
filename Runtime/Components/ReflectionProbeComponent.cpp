#include "Components/ReflectionProbeComponent.h"
#include "Engine/GameObject.h"
#include "ECS/TransformECS.h"

#include <algorithm>
#include <bit>
#include <cmath>

using namespace Sailor;

void ReflectionProbeComponent::Initialize()
{
	m_handle = GetWorld()->GetECS<ReflectionsECS>()->RegisterComponent();
	auto& data = GetWorld()->GetECS<ReflectionsECS>()->GetComponentData(m_handle);
	data.SetOwner(GetOwner());
	ApplyConfiguration();
}

void ReflectionProbeComponent::EndPlay()
{
	if (m_handle != ECS::InvalidIndex)
	{
		GetWorld()->GetECS<ReflectionsECS>()->UnregisterComponent(m_handle);
		m_handle = ECS::InvalidIndex;
	}
}

const ReflectionProbeData& ReflectionProbeComponent::GetData() const
{
	return GetWorld()->GetECS<ReflectionsECS>()->GetComponentData(m_handle);
}

void ReflectionProbeComponent::ApplyConfiguration()
{
	if (m_handle != ECS::InvalidIndex)
	{
		auto& data = GetWorld()->GetECS<ReflectionsECS>()->GetComponentData(m_handle);
		data.m_configuration = m_configuration;
		data.MarkDirty();
	}
}

void ReflectionProbeComponent::OnGizmo()
{
	const auto& matrix = GetOwner()->GetTransformComponent().GetCachedWorldMatrix();
	GetWorld()->GetDebugContext()->DrawSphere(glm::vec3(matrix[3]),
		CalculateReflectionProbeWorldRadius(m_configuration.m_radius, matrix),
		glm::vec4(0.2f, 0.8f, 1.0f, 1.0f));
}

void ReflectionProbeComponent::SetReflectionMap(const TexturePtr& value)
{
	if (m_configuration.m_reflectionMap != value)
	{
		m_configuration.m_reflectionMap = value;
		ApplyConfiguration();
	}
}

void ReflectionProbeComponent::SetUpdateMode(EReflectionProbeUpdateMode value)
{
	switch (value)
	{
	case EReflectionProbeUpdateMode::Static:
	case EReflectionProbeUpdateMode::OnLoad:
	case EReflectionProbeUpdateMode::EveryFrame:
	case EReflectionProbeUpdateMode::Interval:
		break;
	default:
		value = EReflectionProbeUpdateMode::OnLoad;
		break;
	}
	if (m_configuration.m_updateMode != value)
	{
		m_configuration.m_updateMode = value;
		ApplyConfiguration();
	}
}

void ReflectionProbeComponent::SetUpdateInterval(float value)
{
	value = std::isfinite(value) ? (std::max)(value, 0.01f) : 20.0f;
	if (m_configuration.m_updateInterval != value)
	{
		m_configuration.m_updateInterval = value;
		ApplyConfiguration();
	}
}

void ReflectionProbeComponent::SetResolution(uint32_t value)
{
	value = std::bit_ceil(std::clamp(value, 16u, 512u));
	if (m_configuration.m_resolution != value)
	{
		m_configuration.m_resolution = value;
		ApplyConfiguration();
	}
}

void ReflectionProbeComponent::SetRadius(float value)
{
	value = std::isfinite(value) ? (std::max)(value, 0.01f) : 10.0f;
	if (m_configuration.m_radius != value)
	{
		m_configuration.m_radius = value;
		ApplyConfiguration();
	}
}
