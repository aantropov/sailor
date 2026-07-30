#include "Components/SkyComponent.h"
#include "Components/LightComponent.h"
#include "Engine/GameObject.h"
#include "ECS/TransformECS.h"
#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
#include "FrameGraph/SkyNode.h"
#include "RHI/Renderer.h"
#include <glm/gtx/quaternion.hpp>
#include <cmath>

using namespace Sailor;
using namespace Sailor::Framegraph;

namespace
{
	TRefPtr<SkyNode> GetSkyNode()
	{
		auto* renderer = App::GetSubmodule<RHI::Renderer>();
		if (!renderer)
		{
			return {};
		}

		FrameGraphPtr frameGraph = renderer->GetFrameGraph();
		if (!frameGraph)
		{
			return {};
		}

		RHI::RHIFrameGraphPtr rhiFrameGraph = frameGraph->GetRHI();
		if (!rhiFrameGraph)
		{
			return {};
		}

		return rhiFrameGraph->GetGraphNode("Sky").DynamicCast<SkyNode>();
	}

	glm::mat4 CalculateWorldMatrix(GameObjectPtr gameObject)
	{
		if (!gameObject)
		{
			return glm::mat4(1.0f);
		}

		const glm::mat4 localMatrix =
			gameObject->GetTransformComponent().GetTransform().Matrix();
		const GameObjectPtr& parent = gameObject->GetParent();
		return parent
			? CalculateWorldMatrix(parent) * localMatrix
			: localMatrix;
	}

	bool IsFinite(const glm::vec3& value)
	{
		return Math::AllFinite(value);
	}
}

SkyComponent::SkyComponent()
{
	UpdateLightDirection();
}

void SkyComponent::BeginPlay()
{
	Apply();
}

void SkyComponent::EndPlay()
{
	if (auto skyNode = GetSkyNode())
	{
		skyNode->ResetSkyParams();
	}
}

void SkyComponent::Tick(float)
{
	Apply();
}

void SkyComponent::EditorTick(float)
{
	Apply();
}

void SkyComponent::Apply()
{
	UpdateLightDirection();

	if (auto skyNode = GetSkyNode())
	{
		skyNode->SetSkyParams(m_skyParams);
	}

	if (!m_directionalLight)
	{
		if (m_directionalLight.IsInited())
		{
			m_directionalLight.Clear();
		}
		return;
	}

	GameObjectPtr lightOwner = m_directionalLight->GetOwner();
	if (!lightOwner)
	{
		m_directionalLight.Clear();
		return;
	}

	const glm::vec3 worldDirection = Math::SafeNormalize(
		glm::vec3(m_skyParams.m_lightDirection),
		Math::vec3_Down);
	const glm::vec3 worldPosition = -worldDirection * 9000.0f;

	glm::vec3 worldUp = Math::vec3_Up;
	if (glm::abs(glm::dot(worldDirection, worldUp)) > 0.99f)
	{
		worldUp = Math::vec3_Right;
	}

	glm::vec3 localDirection = worldDirection;
	glm::vec3 localUp = worldUp;
	glm::vec3 localPosition = worldPosition;

	if (const GameObjectPtr& parent = lightOwner->GetParent())
	{
		const glm::mat4 inverseParent = glm::inverse(CalculateWorldMatrix(parent));
		localDirection = Math::SafeNormalize(
			glm::mat3(inverseParent) * worldDirection,
			Math::vec3_Down);
		localUp = Math::SafeNormalize(
			glm::mat3(inverseParent) * worldUp,
			Math::vec3_Up);
		localPosition = glm::vec3(inverseParent * glm::vec4(worldPosition, 1.0f));
	}

	if (!IsFinite(localDirection) ||
		!IsFinite(localUp) ||
		!IsFinite(localPosition))
	{
		return;
	}

	if (glm::abs(glm::dot(localDirection, localUp)) > 0.99f)
	{
		localUp = glm::abs(glm::dot(localDirection, Math::vec3_Right)) < 0.99f
			? Math::vec3_Right
			: Math::vec3_Up;
	}

	auto& transform = lightOwner->GetTransformComponent();
	transform.SetRotation(glm::quatLookAt(localDirection, localUp));
	transform.SetPosition(localPosition);

	m_directionalLight->SetLightType(ELightType::Directional);
	m_directionalLight->SetIntensity(
		m_sunAngleDegrees > 0.0f
			? m_directionalLightIntensity
			: glm::vec3(0.0f));
}

void SkyComponent::UpdateLightDirection()
{
	const float sunAngleRadians = glm::radians(m_sunAngleDegrees);
	m_skyParams.m_lightDirection = Math::SafeNormalize(
		glm::vec4(
			0.2f,
			std::sin(-sunAngleRadians),
			std::cos(sunAngleRadians),
			0.0f),
		Math::vec4_Down);
}

void SkyComponent::SetSunAngle(float value)
{
	m_sunAngleDegrees = glm::clamp(value, -25.0f, 89.0f);
	UpdateLightDirection();
}

void SkyComponent::SetCloudsDensity(float value)
{
	m_skyParams.m_cloudsDensity = glm::clamp(value, 0.0f, 1.0f);
}

void SkyComponent::SetCloudsCoverage(float value)
{
	m_skyParams.m_cloudsCoverage = glm::clamp(value, 0.0f, 2.0f);
}

void SkyComponent::SetCloudsAttenuation1(float value)
{
	m_skyParams.m_cloudsAttenuation1 = glm::clamp(value, 0.1f, 0.3f);
}

void SkyComponent::SetCloudsAttenuation2(float value)
{
	m_skyParams.m_cloudsAttenuation2 = glm::clamp(value, 0.001f, 0.1f);
}

void SkyComponent::SetCloudsPhaseInfluence1(float value)
{
	m_skyParams.m_phaseInfluence1 = glm::clamp(value, 0.0f, 1.0f);
}

void SkyComponent::SetCloudsPhaseEccentricity1(float value)
{
	m_skyParams.m_eccentrisy1 = glm::clamp(value, 0.0f, 1.0f);
}

void SkyComponent::SetCloudsPhaseInfluence2(float value)
{
	m_skyParams.m_phaseInfluence2 = glm::clamp(value, 0.0f, 1.0f);
}

void SkyComponent::SetCloudsPhaseEccentricity2(float value)
{
	m_skyParams.m_eccentrisy2 = glm::clamp(value, 0.01f, 1.0f);
}

void SkyComponent::SetCloudsHorizonBlend(float value)
{
	m_skyParams.m_fog = glm::clamp(value, 0.0f, 20.0f);
}

void SkyComponent::SetSunIntensity(float value)
{
	m_skyParams.m_sunIntensity = glm::clamp(value, 0.0f, 800.0f);
}

void SkyComponent::SetAmbient(float value)
{
	m_skyParams.m_ambient = glm::clamp(value, 0.0f, 10.0f);
}

void SkyComponent::SetScatteringSteps(int32_t value)
{
	m_skyParams.m_scatteringSteps = glm::clamp(value, 1, 10);
}

void SkyComponent::SetScatteringDensity(float value)
{
	m_skyParams.m_scatteringDensity = glm::clamp(value, 0.1f, 1.0f);
}

void SkyComponent::SetScatteringIntensity(float value)
{
	m_skyParams.m_scatteringIntensity = glm::clamp(value, 0.01f, 1.0f);
}

void SkyComponent::SetScatteringPhase(float value)
{
	m_skyParams.m_scatteringPhase = glm::clamp(value, 0.001f, 1.0f);
}

void SkyComponent::SetSunShaftsDistance(int32_t value)
{
	m_skyParams.m_sunShaftsDistance = glm::clamp(value, 1, 100);
}

void SkyComponent::SetSunShaftsIntensity(float value)
{
	m_skyParams.m_sunShaftsIntensity = glm::clamp(value, 0.001f, 1.0f);
}

void SkyComponent::SetDirectionalLight(
	const TObjectPtr<LightComponent>& value)
{
	m_directionalLight = value;
}

void SkyComponent::SetDirectionalLightIntensity(const glm::vec3& value)
{
	m_directionalLightIntensity = glm::max(value, glm::vec3(0.0f));
}
