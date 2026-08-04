#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Components/EditorComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Components/CameraComponent.h"
#include "Engine/GameObject.h"
#include "Engine/EngineLoop.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"
#include "ECS/TransformECS.h"
#include <glm/gtc/random.hpp>
#include "Core/Reflection.h"

#include "RHI/Texture.h"
#include "FrameGraph/CopyTextureToRamNode.h"

using namespace Sailor;
using namespace Sailor::Tasks;

void EditorComponent::EditorTick(float deltaTime)
{
	auto& transform = GetOwner()->GetTransformComponent();
	auto syncViewAngles = [&]()
	{
		const vec3 forward = transform.GetTransform().GetForward();
		m_yaw = glm::degrees(glm::atan(forward.x, -forward.z));
		m_pitch = glm::degrees(glm::asin(glm::clamp(forward.y, -1.0f, 1.0f)));
	};

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
		syncViewAngles();
		m_lastCursorPos = GetWorld()->GetInput().GetCursorPos();
		m_bInited = true;
		return;
	}

	const vec3 cameraViewDirection = transform.GetRotation() * Math::vec4_Forward;

	const float sensitivity = 1500;

	const bool bNavigatingViewport = GetWorld()->GetInput().IsKeyDown(VK_RBUTTON);
	const glm::ivec2 cursorPos = GetWorld()->GetInput().GetCursorPos();
	if (!bNavigatingViewport)
	{
		syncViewAngles();
	}
	if (bNavigatingViewport)
	{
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

		const float boost = (GetWorld()->GetInput().IsKeyDown(VK_SHIFT) ? 100.0f : 1.0f) *
			(GetWorld()->GetInput().IsKeyDown(VK_CONTROL) ? 100.0f : 1.0f);
		if (glm::length(delta) > 0)
		{
			const vec4 shift = vec4(Math::SafeNormalize(delta) * boost * sensitivity * deltaTime, 1.0f);
			transform.SetPosition(transform.GetPosition() + shift);
		}

		const vec2 deltaCursorPos = cursorPos - m_lastCursorPos;
		if (deltaCursorPos != vec2(0.0f))
		{
			constexpr float rotationDegreesPerPixel = 0.2f;
			const vec2 rotationDelta = deltaCursorPos * rotationDegreesPerPixel;

			m_yaw += rotationDelta.x;
			m_pitch = glm::clamp(m_pitch - rotationDelta.y, -85.0f, 85.0f);

			glm::quat hRotation = glm::angleAxis(glm::radians(-m_yaw), glm::vec3(0, 1, 0));
			glm::quat vRotation = glm::angleAxis(glm::radians(m_pitch), glm::vec3(1, 0, 0));

			transform.SetRotation(hRotation * vRotation);
		}
	}

	m_lastCursorPos = cursorPos;

}
