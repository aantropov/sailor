#include "Components/Tests/PerformanceTestSetupComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "ECS/TransformECS.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "Math/Math.h"
#include "RHI/Renderer.h"
#include <cfloat>
#include <cmath>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

using namespace Sailor;

namespace
{
	glm::vec3 ColorFromIndex(uint32_t idx, const glm::vec3& baseColor, const glm::vec3& variance)
	{
		const glm::vec3 palette[] =
		{
			glm::vec3(0.45f, 0.05f, 0.05f),
			glm::vec3(0.05f, 0.45f, 0.05f),
			glm::vec3(0.05f, 0.05f, 0.45f),
			glm::vec3(0.45f, 0.45f, 0.05f),
			glm::vec3(0.45f, 0.05f, 0.45f),
			glm::vec3(0.05f, 0.45f, 0.45f),
			glm::vec3(0.45f, 0.25f, 0.05f),
			glm::vec3(0.25f, 0.05f, 0.45f),
		};

		if (glm::length(variance) <= 0.0001f)
		{
			return palette[idx % (sizeof(palette) / sizeof(palette[0]))];
		}

		const float r = ((idx * 97u) % 255u) / 255.0f;
		const float g = ((idx * 57u + 37u) % 255u) / 255.0f;
		const float b = ((idx * 17u + 73u) % 255u) / 255.0f;
		return glm::clamp(baseColor + variance * glm::vec3(r, g, b), glm::vec3(0.0f), glm::vec3(1.0f));
	}
}

void PerformanceTestSetupComponent::BeginPlay()
{
	Component::BeginPlay();
	SpawnGrid();
	SpawnLights();
	EnsureCamera();
}

void PerformanceTestSetupComponent::Tick(float deltaTime)
{
	if (!m_bAppliedRuntimeColors)
	{
		m_bAppliedRuntimeColors = ApplyRuntimeMaterialColors();
	}

	const glm::quat deltaRotation = glm::angleAxis(glm::radians(deltaTime * m_rotationSpeedDeg), Math::vec3_Up);

	for (size_t i = 0; i < m_spawnedObjects.Num(); i++)
	{
		auto& go = m_spawnedObjects[i];
		auto& transform = go->GetTransformComponent();
		const float speed = i < m_perObjectRotationSpeedDeg.Num() ? m_perObjectRotationSpeedDeg[i] : m_rotationSpeedDeg;
		const glm::quat localDeltaRotation = glm::angleAxis(glm::radians(deltaTime * speed), Math::vec3_Up);
		transform.SetRotation(transform.GetRotation() * localDeltaRotation);
	}
}

void PerformanceTestSetupComponent::SpawnGrid()
{
	const float gridHalf = (float)(glm::max(1u, m_gridSize) - 1u) * 0.5f;
	m_spawnedObjects.Clear();
	m_pendingColors.Clear();
	m_perObjectScale.Clear();
	m_perObjectRotationSpeedDeg.Clear();
	m_bPerObjectRuntimeColorApplied.Clear();
	m_bAppliedRuntimeColors = false;

	for (uint32_t x = 0; x < m_gridSize; x++)
	{
		for (uint32_t y = 0; y < m_gridSize; y++)
		{
			for (uint32_t z = 0; z < m_gridSize; z++)
			{
				const uint32_t idx = x * m_gridSize * m_gridSize + y * m_gridSize + z;
				const float scaleJitter = 0.85f + 0.30f * (((idx * 37u + 11u) % 100u) / 100.0f);
				const float rotationSpeed = 6.0f + 28.0f * (((idx * 53u + 17u) % 100u) / 100.0f);
				auto go = GetWorld()->Instantiate("PerfBox");
				auto& transform = go->GetTransformComponent();
				transform.SetPosition(glm::vec4(
					((float)x - gridHalf) * m_spacing,
					((float)y - gridHalf) * m_spacing,
					((float)z - gridHalf) * m_spacing,
					1.0f));
				transform.SetScale(glm::vec4(m_boxScale * scaleJitter, m_boxScale * scaleJitter, m_boxScale * scaleJitter, 1.0f));

				go->SetMobilityType(EMobilityType::Stationary);

				auto mesh = go->AddComponent<MeshRendererComponent>();
				if (!mesh->LoadModel("Models/Box/Box.gltf"))
				{
					continue;
				}

				m_spawnedObjects.Add(go);
				m_pendingColors.Add(glm::vec4(ColorFromIndex(idx, m_baseColor, m_colorVariance), 1.0f));
				m_perObjectScale.Add(scaleJitter);
				m_perObjectRotationSpeedDeg.Add(rotationSpeed);
				m_bPerObjectRuntimeColorApplied.Add(false);
			}
		}
	}
}

bool PerformanceTestSetupComponent::ApplyRuntimeMaterialColors()
{
	auto renderer = App::GetSubmodule<RHI::Renderer>();
	if (!renderer)
	{
		return false;
	}

	auto commands = renderer->GetDriverCommands();
	bool bAllReady = m_spawnedObjects.Num() > 0;

	for (size_t i = 0; i < m_spawnedObjects.Num(); i++)
	{
		auto& go = m_spawnedObjects[i];
		if (!go)
		{
			bAllReady = false;
			continue;
		}

		auto mesh = go->GetComponent<MeshRendererComponent>();
		if (!mesh)
		{
			bAllReady = false;
			continue;
		}

		const glm::vec4 color = i < m_pendingColors.Num() ? m_pendingColors[i] : glm::vec4(1.0f);
		const bool bAlreadyApplied = i < m_bPerObjectRuntimeColorApplied.Num() && m_bPerObjectRuntimeColorApplied[i];
		bool bAnyMaterialUpdated = false;

		for (size_t materialIndex = 0; materialIndex < mesh->GetMaterials().Num(); materialIndex++)
		{
			auto& mat = mesh->GetMaterials()[materialIndex];
			if (!mat || !mat->IsReady() || !mat->GetShaderBindings() || !mat->GetShaderBindings()->HasParameter("material.baseColorFactor"))
			{
				bAllReady = false;
				continue;
			}

			if (!bAlreadyApplied)
			{
				auto instance = Material::CreateInstance(GetWorld(), mat);
				instance->SetUniform("material.baseColorFactor", color);
				commands->SetMaterialParameter(GetWorld()->GetCommandList(), instance->GetShaderBindings(), "material.baseColorFactor", color);
				mat = instance;
			}

			bAnyMaterialUpdated = true;
		}

		if (!bAnyMaterialUpdated)
		{
			bAllReady = false;
			continue;
		}

		if (!bAlreadyApplied && i < m_bPerObjectRuntimeColorApplied.Num())
		{
			m_bPerObjectRuntimeColorApplied[i] = true;
		}
	}

	return bAllReady;
}

void PerformanceTestSetupComponent::SpawnLights()
{
	const float extent = (glm::max(1u, m_gridSize) - 1u) * m_spacing * 0.5f;

	if (m_bSpawnPointLights)
	{
		const glm::vec3 palette[] =
		{
			glm::vec3(1.0f),
			glm::vec3(1.0f),
			glm::vec3(1.0f),
			glm::vec3(1.0f),
			glm::vec3(1.0f),
			glm::vec3(1.0f),
			glm::vec3(1.0f),
			glm::vec3(1.0f),
		};
		const uint32_t lightsCount = 16;
		const float radius = extent * 1.15f;
		for (uint32_t i = 0; i < lightsCount; i++)
		{
			const float angle = glm::two_pi<float>() * ((float)i / (float)lightsCount);
			const float yLayer = (i % 2 == 0) ? extent * 0.20f : extent * 0.55f;
			auto lightGo = GetWorld()->Instantiate("PerfPointLight");
			auto& transform = lightGo->GetTransformComponent();
			transform.SetPosition(glm::vec4(
				cos(angle) * radius,
				yLayer,
				sin(angle) * radius,
				1.0f));

			auto light = lightGo->AddComponent<LightComponent>();
			light->SetLightType(ELightType::Point);
			light->SetIntensity(palette[i % (sizeof(palette) / sizeof(palette[0]))] * 60.0f);
			light->SetBounds(glm::vec3(700.0f));
			light->SetAttenuation(glm::vec3(1.0f, 0.020f, 0.0010f));
			light->SetShadowType(RHI::EShadowType::None);
		}
	}

	auto dirLightGo = GetWorld()->Instantiate("PerfDirectionalLight");
	auto& transform = dirLightGo->GetTransformComponent();
	transform.SetPosition(glm::vec4(0.0f, extent * 1.5f, 0.0f, 1.0f));
	transform.SetRotation(glm::quat(glm::vec3(-0.8f, 0.6f, 0.0f)));
	auto dirLight = dirLightGo->AddComponent<LightComponent>();
	dirLight->SetLightType(ELightType::Directional);
	dirLight->SetIntensity(glm::vec3(m_directionalLightIntensity));
	dirLight->SetShadowType(RHI::EShadowType::PCF);
}

void PerformanceTestSetupComponent::EnsureCamera()
{
	GameObjectPtr cameraGo;
	for (auto& go : GetWorld()->GetGameObjects())
	{
		if (go->GetComponent<CameraComponent>())
		{
			cameraGo = go;
			break;
		}
	}

	if (!cameraGo)
	{
		cameraGo = GetWorld()->Instantiate("PerfCamera");
		cameraGo->AddComponent<CameraComponent>();
	}

	auto& transform = cameraGo->GetTransformComponent();
	const float extent = (glm::max(1u, m_gridSize) - 1u) * m_spacing * 0.5f;
	const glm::vec3 cameraPosition(0.0f, extent * 1.1f, extent * 2.6f);
	transform.SetPosition(glm::vec4(cameraPosition, 1.0f));

	glm::vec3 forward = glm::normalize(-cameraPosition);
	const bool bValidForward = std::isfinite(forward.x) && std::isfinite(forward.y) && std::isfinite(forward.z) && glm::length(forward) >= FLT_EPSILON;
	if (!bValidForward)
	{
		forward = glm::vec3(0.0f, -0.2f, -1.0f);
	}

	const glm::quat rotation = glm::quatLookAt(forward, glm::vec3(0.0f, 1.0f, 0.0f));
	transform.SetRotation(rotation);
}
