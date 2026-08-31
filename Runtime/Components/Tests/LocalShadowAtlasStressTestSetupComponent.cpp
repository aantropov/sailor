#include "Components/Tests/LocalShadowAtlasStressTestSetupComponent.h"
#include "AssetRegistry/AssetRegistry.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Components/SkyComponent.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "Math/Math.h"

#include <cmath>
#include <cstdio>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

using namespace Sailor;

namespace
{
	constexpr uint32_t NumBoxesPerField = 25u;
	constexpr uint32_t NumPointLightsPerField = 300u;
	constexpr float FieldSpacing = 165.0f;
	constexpr float CameraHeight = 62.0f;
	constexpr float CameraDepth = 80.0f;
	constexpr float CameraTravel = 190.0f;
	constexpr float CameraPeriodSeconds = 8.0f;
	const glm::vec3 CameraTarget(0.0f, 2.0f, -330.0f);

	const glm::vec3 LightPalette[] =
	{
		glm::vec3(1.0f, 0.18f, 0.12f),
		glm::vec3(1.0f, 0.48f, 0.12f),
		glm::vec3(1.0f, 0.90f, 0.18f),
		glm::vec3(0.25f, 1.0f, 0.32f),
		glm::vec3(0.12f, 0.82f, 1.0f),
		glm::vec3(0.22f, 0.38f, 1.0f),
		glm::vec3(0.72f, 0.24f, 1.0f),
		glm::vec3(1.0f, 0.20f, 0.68f)
	};
}

void LocalShadowAtlasStressTestSetupComponent::BeginPlay()
{
	Component::BeginPlay();
	SpawnGeometry();
	SpawnLights();
	EnsureSky();
	EnsureCamera();
}

void LocalShadowAtlasStressTestSetupComponent::Tick(float deltaTime)
{
	if (!m_camera)
	{
		return;
	}

	m_elapsedTime += deltaTime;
	const float phase = m_elapsedTime * glm::two_pi<float>() / CameraPeriodSeconds;
	const glm::vec3 position(
		std::sin(phase) * CameraTravel,
		CameraHeight,
		CameraDepth);
	auto& transform = m_camera->GetTransformComponent();
	transform.SetPosition(glm::vec4(position, 1.0f));
	const glm::vec3 target = CameraTarget + glm::vec3(position.x, 0.0f, 0.0f);
	transform.SetRotation(glm::quatLookAt(
		Math::SafeNormalize(target - position, glm::vec3(0.0f, 0.0f, -1.0f)),
		glm::vec3(0.0f, 1.0f, 0.0f)));
}

void LocalShadowAtlasStressTestSetupComponent::SpawnBox(
	const char* name,
	const glm::vec3& position,
	const glm::vec3& scale)
{
	auto gameObject = GetWorld()->Instantiate(name);
	gameObject->SetMobilityType(EMobilityType::Stationary);
	auto& transform = gameObject->GetTransformComponent();
	transform.SetPosition(glm::vec4(position, 1.0f));
	transform.SetScale(glm::vec4(scale, 1.0f));

	auto mesh = gameObject->AddComponent<MeshRendererComponent>();
	mesh->LoadModel("Models/Box/Box.gltf");
	if (auto materialInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(
		"Models/Box/materials/Default.mat"))
	{
		mesh->SetOverrideMaterials({ materialInfo->GetFileId() });
	}
}

void LocalShadowAtlasStressTestSetupComponent::SpawnGeometry()
{
	SpawnBox("ShadowStressFloor", glm::vec3(0.0f, -2.0f, -380.0f), glm::vec3(275.0f, 2.0f, 470.0f));

	SpawnGeometryField("Left", -FieldSpacing, 0u);
	SpawnGeometryField("Center", 0.0f, 1u);
	SpawnGeometryField("Right", FieldSpacing, 2u);
}

void LocalShadowAtlasStressTestSetupComponent::SpawnGeometryField(
	const char* namePrefix,
	float xOffset,
	uint32_t fieldIndex)
{
	for (uint32_t i = 0; i < NumBoxesPerField; ++i)
	{
		const uint32_t variedIndex = i + fieldIndex * 7u;
		const uint32_t lane = (variedIndex * 3u) % 5u;
		const float height = 10.0f + static_cast<float>((variedIndex * 7u) % 5u) * 3.0f;
		const float width = 8.0f + static_cast<float>((variedIndex * 11u) % 4u) * 1.5f;
		char name[64];
		sprintf_s(name, sizeof(name), "ShadowStressBox_%s_%02u", namePrefix, i);
		SpawnBox(
			name,
			glm::vec3(
				xOffset + (static_cast<float>(lane) - 2.0f) * 27.5f,
				height * 0.5f,
				-45.0f - static_cast<float>(i) * 29.0f),
			glm::vec3(width, height, width));
	}
}

void LocalShadowAtlasStressTestSetupComponent::SpawnLights()
{
	SpawnLightField("Left", -FieldSpacing, 0u);
	SpawnLightField("Center", 0.0f, 1u);
	SpawnLightField("Right", FieldSpacing, 2u);
}

void LocalShadowAtlasStressTestSetupComponent::SpawnLightField(
	const char* namePrefix,
	float xOffset,
	uint32_t fieldIndex)
{
	constexpr uint32_t LightsAcross = 5u;
	constexpr uint32_t LightsHigh = 3u;
	constexpr uint32_t LightsDeep = 20u;
	static_assert(LightsAcross * LightsHigh * LightsDeep == NumPointLightsPerField);

	for (uint32_t depth = 0; depth < LightsDeep; ++depth)
	{
		for (uint32_t height = 0; height < LightsHigh; ++height)
		{
			for (uint32_t across = 0; across < LightsAcross; ++across)
			{
				const uint32_t index =
					depth * LightsHigh * LightsAcross + height * LightsAcross + across;
				char name[64];
				sprintf_s(name, sizeof(name), "ShadowStressPointLight_%s_%03u", namePrefix, index);

				auto gameObject = GetWorld()->Instantiate(name);
				gameObject->SetMobilityType(EMobilityType::Stationary);
				gameObject->GetTransformComponent().SetPosition(glm::vec4(
					xOffset - 55.0f + static_cast<float>(across) * 27.5f,
					10.0f + static_cast<float>(height) * 14.0f,
					-20.0f - static_cast<float>(depth) * 40.0f,
					1.0f));

				auto light = gameObject->AddComponent<LightComponent>();
				light->SetLightType(ELightType::Point);
				light->SetIntensity(LightPalette[
					(index + fieldIndex * 3u) % (sizeof(LightPalette) / sizeof(LightPalette[0]))] * 2000.0f);
				light->SetRadius(65.0f);
				light->SetShadowType(RHI::EShadowType::PCF);
				light->SetShadowQuality(ELightShadowQuality::VeryLow);
			}
		}
	}
}

void LocalShadowAtlasStressTestSetupComponent::EnsureSky()
{
	auto gameObject = GetWorld()->Instantiate("ShadowStressSky");
	auto sky = gameObject->AddComponent<SkyComponent>();
	sky->SetAmbient(0.02f);
	sky->SetCloudScatteringScale(0.0f);
	sky->SetCloudsCoverage(0.0f);
}

void LocalShadowAtlasStressTestSetupComponent::EnsureCamera()
{
	m_camera = GetWorld()->Instantiate("ShadowStressCamera");
	auto camera = m_camera->AddComponent<CameraComponent>();
	camera->SetZNear(0.1f);
	camera->SetZFar(750.0f);
	Tick(0.0f);
}
