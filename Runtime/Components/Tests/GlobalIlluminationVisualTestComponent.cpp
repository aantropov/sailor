#include "Components/Tests/GlobalIlluminationVisualTestComponent.h"

#include "AssetRegistry/AssetRegistry.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Components/SkyComponent.h"
#include "ECS/GlobalIlluminationECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "Math/Math.h"

#include <cmath>
#include <glm/gtc/quaternion.hpp>

using namespace Sailor;

namespace
{
	constexpr uint32_t TransitionFrames = 120u;
	constexpr uint32_t ValidationGraceFrames = 45u;
}

void GlobalIlluminationVisualTestComponent::BeginPlay()
{
	VisualTestCaseComponent::BeginPlay();

	SpawnBox(
		"GiFloor",
		glm::vec3(0.0f, -5.0f, 0.0f),
		glm::vec3(70.0f, 2.0f, 70.0f),
		EMobilityType::Static);
	SpawnBox(
		"GiBackWall",
		glm::vec3(0.0f, 25.0f, -52.0f),
		glm::vec3(70.0f, 30.0f, 2.0f),
		EMobilityType::Static);
	SpawnBox(
		"GiLeftReceiver",
		glm::vec3(-25.0f, 6.0f, -5.0f),
		glm::vec3(9.0f, 11.0f, 9.0f),
		EMobilityType::Stationary);
	SpawnBox(
		"GiCenterReceiver",
		glm::vec3(0.0f, 11.0f, -15.0f),
		glm::vec3(10.0f, 16.0f, 10.0f),
		EMobilityType::Static);
	m_dynamicReceiver = SpawnBox(
		"GiDynamicReceiver",
		glm::vec3(25.0f, 5.0f, 2.0f),
		glm::vec3(8.0f, 10.0f, 8.0f),
		EMobilityType::Dynamic);
	EnsureSkyAndCamera();
}

void GlobalIlluminationVisualTestComponent::Tick(float deltaTime)
{
	if (IsFinished())
	{
		return;
	}

	++m_totalFrames;
	m_elapsedTime += deltaTime;
	if (m_dynamicReceiver)
	{
		m_dynamicReceiver->GetTransformComponent().SetPosition(glm::vec4(
			25.0f + std::sin(m_elapsedTime * 1.5f) * 4.0f,
			5.0f,
			2.0f,
			1.0f));
	}

	auto* globalIllumination = GetWorld() ?
		GetWorld()->GetECS<GlobalIlluminationECS>() : nullptr;
	if (!globalIllumination)
	{
		MarkFailed("Global Illumination ECS is unavailable.");
		return;
	}

	if (!m_bTransitionStarted && AreProbeStatesResident())
	{
		m_bTransitionStarted = true;
		m_transitionStartFrame = m_totalFrames;
		m_initialCompositionCount =
			globalIllumination->GetCompositionCount();
		AddJournalEvent(
			"GiStatesResident",
			"Starting Day to Evening to Night interpolation.");
	}

	if (m_bTransitionStarted && m_transitionFrame < TransitionFrames)
	{
		const float progress = static_cast<float>(m_transitionFrame) /
			static_cast<float>(TransitionFrames - 1u);
		TMap<std::string, float> weights;
		if (progress < 0.5f)
		{
			const float evening = progress * 2.0f;
			weights.Insert("Day", 1.0f - evening);
			weights.Insert("Evening", evening);
			weights.Insert("Night", 0.0f);
		}
		else
		{
			const float night = (progress - 0.5f) * 2.0f;
			weights.Insert("Day", 0.0f);
			weights.Insert("Evening", 1.0f - night);
			weights.Insert("Night", night);
		}
		weights.Insert("Lamps", 0.25f);
		std::string diagnostic;
		if (!globalIllumination->SetProbeWeights(weights, diagnostic))
		{
			MarkFailed(
				"Global illumination interpolation was rejected: " + diagnostic);
			return;
		}
		++m_transitionFrame;
	}
	else if (m_bTransitionStarted && !m_bFinalSnapshotValidated)
	{
		std::string diagnostic;
		if (ValidateFinalSnapshot(diagnostic))
		{
			m_bFinalSnapshotValidated = true;
			AddJournalEvent(
				"GiInterpolationComplete",
				"Night Blend plus Lamps Additive snapshot is active.");
		}
		else if (m_transitionStartFrame + TransitionFrames +
			ValidationGraceFrames < m_totalFrames)
		{
			MarkFailed(diagnostic);
			return;
		}
	}

	if (m_totalFrames + 20u >= GetCaptureAfterFrames() &&
		!m_bFinalSnapshotValidated)
	{
		MarkFailed(
			"Global illumination states did not finish interpolation before capture.");
		return;
	}

	VisualTestCaseComponent::Tick(deltaTime);
}

GameObjectPtr GlobalIlluminationVisualTestComponent::SpawnBox(
	const char* name,
	const glm::vec3& position,
	const glm::vec3& scale,
	EMobilityType mobility)
{
	auto gameObject = GetWorld()->Instantiate(name);
	gameObject->SetMobilityType(mobility);
	gameObject->GetTransformComponent().SetPosition(glm::vec4(position, 1.0f));
	gameObject->GetTransformComponent().SetScale(glm::vec4(scale, 1.0f));
	auto mesh = gameObject->AddComponent<MeshRendererComponent>();
	mesh->LoadModel("Models/Box/Box.gltf");
	if (auto materialInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(
		"Models/Box/materials/Default.mat"))
	{
		mesh->SetOverrideMaterials({ materialInfo->GetFileId() });
	}
	return gameObject;
}

void GlobalIlluminationVisualTestComponent::EnsureSkyAndCamera()
{
	auto lightObject = GetWorld()->Instantiate("GiVisualTestDirectionalLight");
	auto directionalLight = lightObject->AddComponent<LightComponent>();
	directionalLight->SetLightType(ELightType::Directional);
	directionalLight->SetShadowType(RHI::EShadowType::PCF);
	directionalLight->SetIndirectLightingIntensity(1.0f);

	auto skyObject = GetWorld()->Instantiate("GiVisualTestSky");
	auto sky = skyObject->AddComponent<SkyComponent>();
	sky->SetAmbient(0.0f);
	sky->SetSunIntensity(0.0f);
	sky->SetCloudsCoverage(0.0f);
	sky->SetDirectionalLight(directionalLight);

	auto cameraObject = GetWorld()->Instantiate("GiVisualTestCamera");
	const glm::vec3 position(0.0f, 32.0f, 108.0f);
	cameraObject->GetTransformComponent().SetPosition(glm::vec4(position, 1.0f));
	cameraObject->GetTransformComponent().SetRotation(glm::quatLookAt(
		Math::SafeNormalize(
			glm::vec3(0.0f, 5.0f, -8.0f) - position,
			glm::vec3(0.0f, 0.0f, -1.0f)),
		glm::vec3(0.0f, 1.0f, 0.0f)));
	auto camera = cameraObject->AddComponent<CameraComponent>();
	camera->SetZNear(0.1f);
	camera->SetZFar(500.0f);
}

bool GlobalIlluminationVisualTestComponent::AreProbeStatesResident() const
{
	auto* globalIllumination = GetWorld() ?
		GetWorld()->GetECS<GlobalIlluminationECS>() : nullptr;
	if (!globalIllumination)
	{
		return false;
	}
	const auto states = globalIllumination->GetProbeStates();
	if (states.Num() != 4u)
	{
		return false;
	}
	for (const GlobalIlluminationProbeState& state : states)
	{
		if (state.m_residency != EGlobalIlluminationProbeResidency::Resident)
		{
			return false;
		}
	}
	return true;
}

bool GlobalIlluminationVisualTestComponent::ValidateFinalSnapshot(
	std::string& outDiagnostic) const
{
	auto* globalIllumination = GetWorld() ?
		GetWorld()->GetECS<GlobalIlluminationECS>() : nullptr;
	const GlobalIlluminationSnapshotPtr snapshot = globalIllumination ?
		globalIllumination->GetActiveSnapshot() : GlobalIlluminationSnapshotPtr{};
	if (!snapshot || snapshot->m_states.Num() != 2u)
	{
		outDiagnostic =
			"Final GI snapshot must contain Night Blend and Lamps Additive states.";
		return false;
	}
	bool bHasNight = false;
	bool bHasLamps = false;
	for (const RHI::RHIGlobalIlluminationState& state : snapshot->m_states)
	{
		bHasNight |= state.m_name == "Night" &&
			state.m_mode == EGlobalIlluminationProbeMode::Blend &&
			std::abs(state.m_effectiveWeight - 1.0f) < 0.001f;
		bHasLamps |= state.m_name == "Lamps" &&
			state.m_mode == EGlobalIlluminationProbeMode::Additive &&
			std::abs(state.m_effectiveWeight - 0.25f) < 0.001f;
	}
	if (!bHasNight || !bHasLamps ||
		globalIllumination->GetCompositionCount() <=
			m_initialCompositionCount + 2u)
	{
		outDiagnostic =
			"Final GI snapshot weights or interpolation composition count are invalid.";
		return false;
	}
	outDiagnostic.clear();
	return true;
}
