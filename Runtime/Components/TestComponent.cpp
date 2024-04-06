#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Components/TestComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Engine/GameObject.h"
#include "Engine/EngineLoop.h"
#include "ECS/TransformECS.h"
#include "glm/glm/gtc/random.hpp"
#include "imgui.h"
#include "Core/Reflection.h"

#include "RHI/Texture.h"
#include "Framegraph/CopyTextureToRamNode.h"

using namespace Sailor;
using namespace Sailor::Tasks;

void TestComponent::BeginPlay()
{
	GetWorld()->GetDebugContext()->DrawOrigin(glm::vec4(600, 2, 0, 0), glm::mat4(1), 20.0f, 3000.0f);

	m_mainModel = GetWorld()->Instantiate();
	m_mainModel->GetTransformComponent().SetPosition(vec3(0, 0, 0));
	m_mainModel->GetTransformComponent().SetScale(vec4(1, 1, 1, 1));
	m_mainModel->GetTransformComponent().SetRotation(glm::quat(vec3(0, 0.5f * Math::Pi, 0)));

	auto meshRenderer = m_mainModel->AddComponent<MeshRendererComponent>();
	meshRenderer->LoadModel("Models/Sponza/sponza.obj");
	m_model = meshRenderer->GetModel();

	auto redBox = GetWorld()->Instantiate();
	redBox->GetTransformComponent().SetPosition(vec3(120, 2000, 500));
	redBox->GetTransformComponent().SetScale(vec4(100, 50, 100, 1));
	meshRenderer = redBox->AddComponent<MeshRendererComponent>();
	meshRenderer->LoadModel("Models/Box/Box.gltf");

	for (int32_t i = -1000; i < 1000; i += 32)
	{
		for (int32_t j = -1000; j < 1000; j += 32)
		{
			int k = 10;
			//for (int32_t k = -1000; k < 1000; k += 32) 
			{
				m_boxes.Add(Math::AABB(glm::vec3(i, k, j), glm::vec3(1.0f, 1.0f, 1.0f)));
				const auto& aabb = m_boxes[m_boxes.Num() - 1];

				//GetWorld()->GetDebugContext()->DrawAABB(aabb, glm::vec4(0.2f, 0.8f, 0.2f, 1.0f), 5.0f);

				m_octree.Insert(aabb.GetCenter(), aabb.GetExtents(), aabb);
			}
		}
	}

	/*
	for (int k = 0; k < 4; k++)
		for (int i = 0; i < 4; i++)
			for (int j = 0; j < 4; j++)
			{
				auto gameObject2 = GetWorld()->Instantiate();
				gameObject2->SetMobilityType(EMobilityType::Stationary);
				gameObject2->GetTransformComponent().SetPosition(vec3(j * 3000, k * 3000, i * 2500));
				gameObject2->GetTransformComponent().SetScale(vec4(1, 1, 1, 0));

				auto meshRenderer = gameObject2->AddComponent<MeshRendererComponent>();
				meshRenderer->LoadModel("Models/Sponza/sponza.obj");

				m_objects.Add(gameObject2);
			}
	/**/

	m_dirLight = GetWorld()->Instantiate();
	auto lightComponent = m_dirLight->AddComponent<LightComponent>();
	m_dirLight->GetTransformComponent().SetPosition(vec3(0.0f, 3000.0f, 1000.0f));
	m_dirLight->GetTransformComponent().SetRotation(quat(vec3(-45, 12.5f, 0)));
	lightComponent->SetLightType(ELightType::Directional);

	/*
	ReflectionInfo reflection = lightComponent->GetReflectionInfo();
	ComponentPtr newComponent = Reflection::CreateObject<Component>(reflection.GetTypeInfo(), GetOwner()->GetWorld()->GetAllocator());
	GetOwner()->AddComponentRaw(newComponent);
	newComponent->ApplyReflection(reflection);
	*/

	//Reflection::ApplyReflection(data3.GetRawPtr(), data1);

	/*auto spotLight = GetWorld()->Instantiate();
	lightComponent = spotLight->AddComponent<LightComponent>();
	spotLight->GetTransformComponent().SetPosition(vec3(200.0f, 40.0f, 0.0f));
	spotLight->GetTransformComponent().SetRotation(quat(vec3(-45, 0.0f, 0.0f)));

	lightComponent->SetBounds(vec3(300.0f, 300.0f, 300.0f));
	lightComponent->SetIntensity(vec3(260.0f, 260.0f, 200.0f));
	lightComponent->SetLightType(ELightType::Spot);
	/**/
	/*
	for (int32_t i = -1000; i < 1000; i += 250)
	{
		for (int32_t j = 0; j < 800; j += 250)
		{
			for (int32_t k = -1000; k < 1000; k += 250)
			{
				auto lightGameObject = GetWorld()->Instantiate();
				auto lightComponent = lightGameObject->AddComponent<LightComponent>();
				const float size = 100 + (float)(rand() % 60);

				//lightGameObject->SetMobilityType(EMobilityType::Static);
				lightGameObject->GetTransformComponent().SetPosition(vec3(i, j, k));
				lightComponent->SetBounds(vec3(size, size, size));
				lightComponent->SetIntensity(vec3(rand() % 256, rand() % 256, rand() % 256));

				m_lights.Emplace(std::move(lightGameObject));
				m_lightVelocities.Add(vec4(0));
			}
		}
	}
	/**/
	//m_octree.DrawOctree(*GetWorld()->GetDebugContext(), 10);

	auto& transform = GetOwner()->GetTransformComponent();
	transform.SetPosition(glm::vec4(0.0f, 150.0f, 0.0f, 0.0f));
	//transform.SetRotation(quat(vec3(-45, 12.5f, 0)));
}

void TestComponent::EndPlay()
{
}

#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

void TestComponent::Tick(float deltaTime)
{
	auto commands = App::GetSubmodule<Sailor::RHI::Renderer>()->GetDriverCommands();

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
		const float smoothFactor = 20;
		const float speed = 100.0f;
		const float smoothDeltaTime = GetWorld()->GetSmoothDeltaTime();

		vec2 deltaCursorPos = GetWorld()->GetInput().GetCursorPos() - m_lastCursorPos;
		vec2 shift = deltaCursorPos * speed * smoothDeltaTime;

		float adjustedYawSpeed = shift.x / (cos(glm::radians(m_pitch)) + 0.1f);

		float targetYaw = m_yaw + adjustedYawSpeed;
		float targetPitch = glm::clamp(m_pitch - shift.y, -85.0f, 85.0f);

		m_yaw = glm::mix(m_yaw, targetYaw, std::min(1.0f, smoothFactor * smoothDeltaTime));
		m_pitch = glm::mix(m_pitch, targetPitch, std::min(1.0f, smoothFactor * smoothDeltaTime));

		glm::quat hRotation = glm::angleAxis(glm::radians(-m_yaw), glm::vec3(0, 1, 0));
		glm::quat vRotation = glm::angleAxis(glm::radians(m_pitch), glm::vec3(1, 0, 0));

		transform.SetRotation(hRotation * vRotation);
	}

	if (GetWorld()->GetInput().IsKeyPressed('F'))
	{
		if (auto camera = GetOwner()->GetComponent<CameraComponent>())
		{
			m_cachedFrustum = transform.GetTransform().Matrix();

			Math::Frustum frustum;
			frustum.ExtractFrustumPlanes(transform.GetTransform().Matrix(), camera->GetAspect(), camera->GetFov(), camera->GetZNear(), camera->GetZFar());

			// Testing ortho matrix
			//const glm::mat4 m = glm::orthoRH_NO(-100.0f, 100.0f, -100.0f, 100.0f, 0.0f, 900.0f) * glm::inverse(m_cachedFrustum);
			//frustum.ExtractFrustumPlanes(m);

			m_octree.Trace(frustum, m_culledBoxes);
		}
	}

	for (const auto& aabb : m_culledBoxes)
	{
		GetWorld()->GetDebugContext()->DrawAABB(aabb, glm::vec4(0.2f, 0.8f, 0.2f, 1.0f));
	}

	if (auto camera = GetOwner()->GetComponent<CameraComponent>())
	{
		if (m_cachedFrustum != glm::mat4(1))
		{
			Math::Frustum frustum;
			frustum.ExtractFrustumPlanes(m_cachedFrustum, camera->GetAspect(), camera->GetFov(), camera->GetZNear(), camera->GetZFar());
			GetWorld()->GetDebugContext()->DrawFrustum(frustum);

			//const auto& lightView = glm::inverse(m_dirLight->GetTransformComponent().GetCachedWorldMatrix());
			//GetWorld()->GetDebugContext()->DrawLightCascades(lightView, m_cachedFrustum, camera->GetAspect(), camera->GetFov(), camera->GetZNear(), camera->GetZFar());

			//const glm::mat4 m = glm::orthoRH_NO(-100.0f, 100.0f, -100.0f, 100.0f, 0.0f, 900.0f) * glm::inverse(m_cachedFrustum);
			//Math::Frustum frustum;
			//frustum.ExtractFrustumPlanes(m);
			//GetWorld()->GetDebugContext()->DrawFrustum(frustum);
			/**/
		}
	}

	m_lastCursorPos = GetWorld()->GetInput().GetCursorPos();

	for (uint32_t i = 0; i < m_lights.Num(); i++)
	{
		if (glm::length(m_lightVelocities[i]) < 1.0f)
		{
			const glm::vec4 velocity = glm::vec4(glm::sphericalRand(75.0f + rand() % 50), 0.0f);
			m_lightVelocities[i] = velocity;
		}

		const auto& position = m_lights[i]->GetTransformComponent().GetWorldPosition();

		m_lightVelocities[i] = Math::Lerp(m_lightVelocities[i], vec4(0, 0, 0, 0), deltaTime * 0.5f);
		m_lights[i]->GetTransformComponent().SetPosition(position + deltaTime * m_lightVelocities[i]);
	}

	for (auto& go : m_objects)
	{
		auto r = go->GetTransformComponent().GetRotation();
		r *= angleAxis(glm::radians(deltaTime * 10.0f), Math::vec3_Up);
		go->GetTransformComponent().SetRotation(r);
	}

	if (GetWorld()->GetInput().IsKeyPressed('R'))
	{
		for (auto& go : GetWorld()->GetGameObjects())
		{
			if (auto mr = go->GetComponent<MeshRendererComponent>())
			{
				for (auto& mat : mr->GetMaterials())
				{
					if (mat && mat->IsReady() && mat->GetShaderBindings()->HasParameter("material.albedo"))
					{
						mat = Material::CreateInstance(GetWorld(), mat);

						const glm::vec4 color = max(vec4(0), glm::vec4(glm::ballRand(1.0f), 1));

						commands->SetMaterialParameter(GetWorld()->GetCommandList(),
							mat->GetShaderBindings(), "material.albedo", color);
					}
				}
			}
		}
	}

	if (auto node = App::GetSubmodule<RHI::Renderer>()->GetFrameGraph()->GetRHI()->GetGraphNode("Sky"))
	{
		auto sky = node.DynamicCast<Framegraph::SkyNode>();

		SkyNode::SkyParams& skyParams = sky->GetSkyParams();

		ImGui::Begin("Screenshot");
		if (ImGui::Button("Capture"))
		{
			if (auto snapshotNode = App::GetSubmodule<RHI::Renderer>()->GetFrameGraph()->GetRHI()->GetGraphNode("CopyTextureToRam"))
			{
				auto snapshot = snapshotNode.DynamicCast<Framegraph::CopyTextureToRamNode>();
				snapshot->DoOneCapture();
			}

			if (auto snapshotNode = *App::GetSubmodule<RHI::Renderer>()->GetFrameGraph()->GetRHI()->GetGraph().Last())
			{
				auto snapshot = snapshotNode.DynamicCast<Framegraph::CopyTextureToRamNode>();
				snapshot->DoOneCapture();
			}
		}

		const bool bSaveMask = ImGui::Button("Save Mask");
		const bool bSave = ImGui::Button("Save");

		FrameGraphNodePtr snapshotNode = nullptr;

		if (bSave)
		{
			snapshotNode = App::GetSubmodule<RHI::Renderer>()->GetFrameGraph()->GetRHI()->GetGraphNode("CopyTextureToRam");
		}
		else if (bSaveMask)
		{
			snapshotNode = *App::GetSubmodule<RHI::Renderer>()->GetFrameGraph()->GetRHI()->GetGraph().Last();
		}

		if (bSave || bSaveMask)
		{
			auto snapshot = snapshotNode.DynamicCast<Framegraph::CopyTextureToRamNode>();
			auto cpuRam = snapshot->GetBuffer();
			auto texture = snapshot->GetTexture();
			if (vec4* ptr = (vec4*)cpuRam->GetPointer())
			{
				TVector<u8vec3> outSrgb(texture->GetExtent().x * texture->GetExtent().y);

				for (int y = 0; y < texture->GetExtent().y; y++)
				{
					for (int x = 0; x < texture->GetExtent().x; x++)
					{
						vec2 uv = vec2((float)x / texture->GetExtent().x, (float)y / texture->GetExtent().y);

						uint32_t index = x + y * texture->GetExtent().x;
						vec3 value = ptr[index];
						value.x = powf(value.x, 1.0f / 2.2f);
						value.y = powf(value.y, 1.0f / 2.2f);
						value.z = powf(value.z, 1.0f / 2.2f);

						outSrgb[index] = u8vec3(glm::clamp(value * 255.0f, 0.0f, 255.0f));
					}
				}

				const uint32_t Channels = 3;
				if (!stbi_write_png(bSave ? "screenshot.png" : "mask.png",
					texture->GetExtent().x, texture->GetExtent().y, Channels, outSrgb.GetData(), texture->GetExtent().x * Channels))
				{
					SAILOR_LOG_ERROR("Cannot write screenshot");
				}
			}
		}

		ImGui::End();

		ImGui::Begin("Sky Settings");
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
		m_dirLight->GetTransformComponent().SetRotation(glm::quatLookAt(skyParams.m_lightDirection.xyz(), Math::vec3_Up));
		m_dirLight->GetTransformComponent().SetPosition(lightPosition);
		m_dirLight->GetComponent<LightComponent>()->SetIntensity(m_sunAngleRad > 0 ? vec3(17.0f, 17.0f, 17.0f) : vec3(0));
	}
}
