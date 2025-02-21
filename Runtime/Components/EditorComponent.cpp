#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Components/EditorComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Engine/GameObject.h"
#include "Engine/EngineLoop.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"
#include "ECS/TransformECS.h"
#include <glm/gtc/random.hpp>
#include "imgui.h"
#include "Core/Reflection.h"

#include "RHI/Texture.h"
#include "FrameGraph/CopyTextureToRamNode.h"

using namespace Sailor;
using namespace Sailor::Tasks;

void EditorComponent::EditorTick(float deltaTime)
{
	if (!m_bInited)
	{
		auto& debugContext = GetWorld()->GetDebugContext();

		const float maxDuration = std::numeric_limits<float>::max();
		glm::vec4 gridColor = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);

		const uint32_t cellCount = 100;
		const float step = 500.0f;

		int32_t halfCount = cellCount / 2;

		for (int32_t i = -halfCount; i <= halfCount; i++)
		{
			float offset = i * step;

			debugContext->DrawLine(
				glm::vec3(-halfCount * step, 0.0f, offset),
				glm::vec3(halfCount * step, 0.0f, offset),
				gridColor, maxDuration);

			debugContext->DrawLine(
				glm::vec3(offset, 0.0f, -halfCount * step),
				glm::vec3(offset, 0.0f, halfCount * step),
				gridColor, maxDuration);
		}

		debugContext->DrawOrigin(glm::vec4(0, 0, 0, 1), glm::mat4(1), 1000.0f, maxDuration);
		m_bInited = true;
		return;
	}

	//auto commands = App::GetSubmodule<Sailor::RHI::Renderer>()->GetDriverCommands();
	//ImGui::ShowDemoWindow();

	auto& transform = GetOwner()->GetTransformComponent();
	const vec3 cameraViewDirection = transform.GetRotation() * Math::vec4_Forward;

	const float sensitivity = 1500;

	glm::vec3 delta = glm::vec3(0.0f, 0.0f, 0.0f);
	if (GetWorld()->GetInput().IsKeyDown('A'))
		delta += -cross(cameraViewDirection, Math::vec3_Up);

	if (GetWorld()->GetInput().IsKeyDown('D'))
		delta += cross(cameraViewDirection, Math::vec3_Up);

	if (GetWorld()->GetInput().IsKeyDown('W'))
		delta += cameraViewDirection;

	if (GetWorld()->GetInput().IsKeyDown('S'))
		delta += -cameraViewDirection;

	if (GetWorld()->GetInput().IsKeyDown('X'))
		delta += vec3(1, 0, 0);

	if (GetWorld()->GetInput().IsKeyDown('Y'))
		delta += vec3(0, 1, 0);

	if (GetWorld()->GetInput().IsKeyDown('Z'))
		delta += vec3(0, 0, 1);

	const float boost = (GetWorld()->GetInput().IsKeyDown(VK_SHIFT) ? 100.0f : 1.0f) * (GetWorld()->GetInput().IsKeyDown(VK_CONTROL) ? 100.0f : 1.0f);

	if (glm::length(delta) > 0)
	{
		vec4 shift = vec4(glm::normalize(delta) * boost * sensitivity * deltaTime, 1.0f);

		const vec4 newPosition = transform.GetPosition() + shift;

		transform.SetPosition(newPosition);
	}

	if (GetWorld()->GetInput().IsKeyDown(VK_LBUTTON))
	{
		const float smoothFactor = 0.9f;
		const float speed = 50.0f;
		const float smoothDeltaTime = GetWorld()->GetSmoothDeltaTime();

		vec2 deltaCursorPos = GetWorld()->GetInput().GetCursorPos() - m_lastCursorPos;
		vec2 shift = deltaCursorPos * speed * smoothDeltaTime;

		float adjustedYawSpeed = shift.x / (cos(glm::radians(m_pitch)) + 0.1f);

		float targetYaw = m_yaw + adjustedYawSpeed;
		float targetPitch = glm::clamp(m_pitch - shift.y, -85.0f, 85.0f);

		m_yaw = glm::mix(m_yaw, targetYaw, smoothFactor);
		m_pitch = glm::mix(m_pitch, targetPitch, smoothFactor);

		glm::quat hRotation = glm::angleAxis(glm::radians(-m_yaw), glm::vec3(0, 1, 0));
		glm::quat vRotation = glm::angleAxis(glm::radians(m_pitch), glm::vec3(1, 0, 0));

		transform.SetRotation(hRotation * vRotation);
	}

	m_lastCursorPos = GetWorld()->GetInput().GetCursorPos();

	if (auto node = App::GetSubmodule<RHI::Renderer>()->GetFrameGraph()->GetRHI()->GetGraphNode("Sky"))
	{
		auto sky = node.DynamicCast<Framegraph::SkyNode>();

		SkyNode::SkyParams& skyParams = sky->GetSkyParams();

		ImGui::Begin("Editor Sky Settings");
		ImGui::SliderAngle("Sun angle", &m_sunAngleRad, -25.0f, 89.0f, "%.2f", ImGuiSliderFlags_::ImGuiSliderFlags_NoRoundToFormat);
		ImGui::SliderFloat("Clouds density", &skyParams.m_cloudsDensity, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_::ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_::ImGuiSliderFlags_NoRoundToFormat);
		ImGui::SliderFloat("Clouds coverage", &skyParams.m_cloudsCoverage, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_::ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_::ImGuiSliderFlags_NoRoundToFormat);
		ImGui::SliderFloat("Clouds attenuation 1", &skyParams.m_cloudsAttenuation1, 0.1f, 0.3f, "%.3f", ImGuiSliderFlags_::ImGuiSliderFlags_NoRoundToFormat);
		ImGui::SliderFloat("Clouds attenuation 2", &skyParams.m_cloudsAttenuation2, 0.001f, 0.1f, "%.3f", ImGuiSliderFlags_::ImGuiSliderFlags_NoRoundToFormat);
		ImGui::SliderFloat("Clouds phase influence 1", &skyParams.m_phaseInfluence1, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_::ImGuiSliderFlags_NoRoundToFormat);
		ImGui::SliderFloat("Clouds phase eccentrisy 1", &skyParams.m_eccentrisy1, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_::ImGuiSliderFlags_NoRoundToFormat);
		ImGui::SliderFloat("Clouds phase influence 2", &skyParams.m_phaseInfluence2, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_::ImGuiSliderFlags_NoRoundToFormat);
		ImGui::SliderFloat("Clouds phase eccentrisy 2", &skyParams.m_eccentrisy2, 0.01f, 1.0f, "%.3f", ImGuiSliderFlags_::ImGuiSliderFlags_NoRoundToFormat);
		ImGui::SliderFloat("Clouds horizon blend", &skyParams.m_fog, 0.0f, 20.0f, "%.3f", ImGuiSliderFlags_::ImGuiSliderFlags_NoRoundToFormat);
		ImGui::SliderFloat("Clouds sun intensity", &skyParams.m_sunIntensity, 0.0f, 800.0f, "%.3f", ImGuiSliderFlags_::ImGuiSliderFlags_NoRoundToFormat);
		ImGui::SliderFloat("Clouds ambient", &skyParams.m_ambient, 0.0f, 10.0f, "%.3f", ImGuiSliderFlags_::ImGuiSliderFlags_NoRoundToFormat);
		ImGui::SliderInt("Clouds scattering steps", &skyParams.m_scatteringSteps, 1, 10, "%d");
		ImGui::SliderFloat("Clouds scattering density", &skyParams.m_scatteringDensity, 0.1f, 1.0f, "%.3f", ImGuiSliderFlags_::ImGuiSliderFlags_NoRoundToFormat);
		ImGui::SliderFloat("Clouds scattering intensity", &skyParams.m_scatteringIntensity, 0.01f, 1.0f, "%.3f", ImGuiSliderFlags_::ImGuiSliderFlags_NoRoundToFormat);
		ImGui::SliderFloat("Clouds scattering phase", &skyParams.m_scatteringPhase, 0.001f, 1.0f, "%.3f", ImGuiSliderFlags_::ImGuiSliderFlags_NoRoundToFormat);
		ImGui::SliderInt("Sun Shafts Distance", &skyParams.m_sunShaftsDistance, 1, 100, "%d");
		ImGui::SliderFloat("Sun Shafts Intensity", &skyParams.m_sunShaftsIntensity, 0.001f, 1.0f, "%.3f", ImGuiSliderFlags_::ImGuiSliderFlags_NoRoundToFormat);
		ImGui::End();

		if (m_skyHash != skyParams.GetHash())
		{
			sky->MarkDirty();
			m_skyHash = skyParams.GetHash();
		}

		glm::vec4 lightPosition = (-skyParams.m_lightDirection) * 9000.0f;

		skyParams.m_lightDirection = normalize(vec4(0.2f, std::sin(-m_sunAngleRad), std::cos(m_sunAngleRad), 0));

		if (m_mainLight)
		{
			m_mainLight->GetTransformComponent().SetRotation(glm::quatLookAt(skyParams.m_lightDirection.xyz(), Math::vec3_Up));
			m_mainLight->GetTransformComponent().SetPosition(lightPosition);
			m_mainLight->GetComponent<LightComponent>()->SetIntensity(m_sunAngleRad > 0 ? vec3(17.0f, 17.0f, 17.0f) : vec3(0));
		}
		else
		{
			auto index = GetWorld()->GetGameObjects().FindIf([](const GameObjectPtr& el) mutable { return el->GetComponent<LightComponent>().IsInited(); });

			if (index != -1)
			{
				m_mainLight = GetWorld()->GetGameObjects()[index];
			}
		}
	}
}
