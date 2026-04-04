#include "Components/Tests/PerformanceTestSetupComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/Tests/TestCaseComponent.h"
#include "ECS/TransformECS.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "Math/Math.h"
#include "RHI/Renderer.h"
#include <cfloat>
#include <cmath>
#include <sstream>
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
	m_minFps = FLT_MAX;
	m_maxFps = 0.0f;
	m_sumFps = 0.0;
	m_numFpsSamples = 0;
	SpawnGrid();
	SpawnLights();
	EnsureCamera();
}

void PerformanceTestSetupComponent::EndPlay()
{
	if (m_numFpsSamples > 0)
	{
		const double avgFps = m_sumFps / (double)m_numFpsSamples;
		std::ostringstream ss;
		ss.setf(std::ios::fixed);
		ss.precision(2);
		ss << "min=" << m_minFps
		   << " avg=" << (float)avgFps
		   << " max=" << m_maxFps
		   << " samples=" << (unsigned long long)m_numFpsSamples;
		const std::string fpsSummary = ss.str();

		if (auto owner = GetOwner())
		{
			if (auto testCase = owner->GetComponent<TestCaseComponent>())
			{
				testCase->AddJournalEvent("PerformanceStats", fpsSummary, 0);
			}
			else
			{
				SAILOR_LOG("PerformanceTest FPS: %s", fpsSummary.c_str());
			}
		}
		else
		{
			SAILOR_LOG("PerformanceTest FPS: %s", fpsSummary.c_str());
		}
	}

	Component::EndPlay();
}

void PerformanceTestSetupComponent::Tick(float deltaTime)
{
	if (deltaTime > FLT_EPSILON)
	{
		const float fps = 1.0f / deltaTime;
		m_minFps = (std::min)(m_minFps, fps);
		m_maxFps = (std::max)(m_maxFps, fps);
		m_sumFps += fps;
		m_numFpsSamples++;
	}

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
			glm::vec3(1.0f, 0.3f, 0.3f),
			glm::vec3(0.3f, 1.0f, 0.3f),
			glm::vec3(0.3f, 0.5f, 1.0f),
			glm::vec3(1.0f, 0.9f, 0.3f),
			glm::vec3(1.0f, 0.3f, 1.0f),
			glm::vec3(0.3f, 1.0f, 1.0f),
			glm::vec3(1.0f, 0.6f, 0.2f),
			glm::vec3(0.8f, 0.8f, 1.0f),
		};

		const uint32_t lightGrid = 4;
		const glm::vec3 minPos(-extent * 0.75f, extent * 0.10f, -extent * 0.75f);
		const glm::vec3 maxPos(extent * 0.75f, extent * 0.85f, extent * 0.75f);

		for (uint32_t z = 0; z < lightGrid; z++)
		{
			for (uint32_t y = 0; y < lightGrid; y++)
			{
				for (uint32_t x = 0; x < lightGrid; x++)
				{
					const glm::vec3 t(
						lightGrid > 1 ? (float)x / (float)(lightGrid - 1) : 0.5f,
						lightGrid > 1 ? (float)y / (float)(lightGrid - 1) : 0.5f,
						lightGrid > 1 ? (float)z / (float)(lightGrid - 1) : 0.5f);

					auto lightGo = GetWorld()->Instantiate("PerfPointLight");
					auto& transform = lightGo->GetTransformComponent();
					transform.SetPosition(glm::vec4(glm::mix(minPos, maxPos, t), 1.0f));

					auto light = lightGo->AddComponent<LightComponent>();
					light->SetLightType(ELightType::Point);
					light->SetIntensity(palette[(x + y * lightGrid + z * lightGrid * lightGrid) % (sizeof(palette) / sizeof(palette[0]))] * 45.0f);
					light->SetBounds(glm::vec3(450.0f));
					light->SetAttenuation(glm::vec3(1.0f, 0.030f, 0.0015f));
					light->SetShadowType(RHI::EShadowType::None);
				}
			}
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
