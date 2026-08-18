#include "Components/Tests/ShadowVisualTestSetupComponent.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Components/SkyComponent.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "Math/Math.h"

#include <glm/gtc/quaternion.hpp>

using namespace Sailor;

void ShadowVisualTestSetupComponent::BeginPlay()
{
	Component::BeginPlay();

	SpawnBox("ShadowFloor", glm::vec3(0.0f, -4.0f, 0.0f), glm::vec3(70.0f, 2.0f, 70.0f));
	SpawnBox("ShadowReceiverWall", glm::vec3(0.0f, 23.0f, -48.0f), glm::vec3(64.0f, 28.0f, 2.0f));
	SpawnBox("ShadowCasterLeft", glm::vec3(-18.0f, 5.0f, 4.0f), glm::vec3(7.0f, 9.0f, 7.0f));
	SpawnBox("ShadowCasterCenter", glm::vec3(0.0f, 9.0f, -4.0f), glm::vec3(8.0f, 13.0f, 8.0f));
	SpawnBox("ShadowCasterRight", glm::vec3(19.0f, 3.0f, 10.0f), glm::vec3(7.0f, 7.0f, 7.0f));

	SpawnLights();
	EnsureSky();
	EnsureCamera();
}

void ShadowVisualTestSetupComponent::SpawnBox(
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

void ShadowVisualTestSetupComponent::SpawnLights()
{
	if (m_bDirectional)
	{
		auto gameObject = GetWorld()->Instantiate("ShadowDirectionalLight");
		gameObject->GetTransformComponent().SetRotation(glm::quatLookAt(
			Math::SafeNormalize(glm::vec3(0.65f, -1.0f, 0.35f), glm::vec3(0.0f, -1.0f, 0.0f)),
			glm::vec3(0.0f, 1.0f, 0.0f)));
		auto light = gameObject->AddComponent<LightComponent>();
		light->SetLightType(ELightType::Directional);
		light->SetIntensity(glm::vec3(4.0f));
		light->SetShadowType(RHI::EShadowType::PCF);
	}

	if (m_bPoint)
	{
		auto gameObject = GetWorld()->Instantiate("ShadowPointLight");
		gameObject->GetTransformComponent().SetPosition(glm::vec4(-38.0f, 30.0f, 34.0f, 1.0f));
		auto light = gameObject->AddComponent<LightComponent>();
		light->SetLightType(ELightType::Point);
		light->SetIntensity(glm::vec3(8.0f, 4.8f, 2.8f));
		light->SetAttenuation(glm::vec3(1.0f, 0.0f, 0.0f));
		light->SetRadius(220.0f);
		light->SetShadowType(RHI::EShadowType::PCF);
		light->SetShadowQuality(ELightShadowQuality::Medium);
	}

	if (m_bSpot)
	{
		auto gameObject = GetWorld()->Instantiate("ShadowSpotLight");
		auto& transform = gameObject->GetTransformComponent();
		const glm::vec3 position(42.0f, 42.0f, 42.0f);
		transform.SetPosition(glm::vec4(position, 1.0f));
		transform.SetRotation(glm::quatLookAt(
			Math::SafeNormalize(-position, glm::vec3(0.0f, -1.0f, 0.0f)),
			glm::vec3(0.0f, 1.0f, 0.0f)));
		auto light = gameObject->AddComponent<LightComponent>();
		light->SetLightType(ELightType::Spot);
		light->SetIntensity(glm::vec3(4.0f, 6.0f, 10.0f));
		light->SetAttenuation(glm::vec3(1.0f, 0.0f, 0.0f));
		light->SetRadius(220.0f);
		light->SetCutOff(glm::vec2(32.0f, 52.0f));
		light->SetShadowType(RHI::EShadowType::PCF);
		light->SetShadowQuality(ELightShadowQuality::Medium);
	}
}

void ShadowVisualTestSetupComponent::EnsureSky()
{
	auto skyObject = GetWorld()->Instantiate("ShadowTestSky");
	auto sky = skyObject->AddComponent<SkyComponent>();
	sky->SetAmbient(0.0f);
	sky->SetSunIntensity(0.0f);
	sky->SetCloudsCoverage(0.0f);
}

void ShadowVisualTestSetupComponent::EnsureCamera()
{
	auto cameraObject = GetWorld()->Instantiate("ShadowTestCamera");
	const glm::vec3 position(0.0f, 34.0f, 105.0f);
	auto& transform = cameraObject->GetTransformComponent();
	transform.SetPosition(glm::vec4(position, 1.0f));
	transform.SetRotation(glm::quatLookAt(
		Math::SafeNormalize(glm::vec3(0.0f, 4.0f, 0.0f) - position, glm::vec3(0.0f, 0.0f, -1.0f)),
		glm::vec3(0.0f, 1.0f, 0.0f)));
	auto camera = cameraObject->AddComponent<CameraComponent>();
	camera->SetZNear(0.1f);
	camera->SetZFar(500.0f);
}
