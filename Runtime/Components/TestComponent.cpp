#include "Components/TestComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Components/CameraComponent.h"
#include "Engine/GameObject.h"
#include "Engine/EngineLoop.h"
#include "ECS/TransformECS.h"

using namespace Sailor;
using namespace Sailor::JobSystem;

void TestComponent::BeginPlay()
{
	m_frameDataBinding = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
	m_perInstanceData = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();

	//bool bNeedsStorageBuffer = m_testMaterial->GetBindings()->NeedsStorageBuffer() ? EShaderBindingType::StorageBuffer : EShaderBindingType::UniformBuffer;
	Sailor::RHI::Renderer::GetDriver()->AddBufferToShaderBindings(m_perInstanceData, "data", sizeof(glm::mat4x4), 0, RHI::EShaderBindingType::StorageBuffer);
	Sailor::RHI::Renderer::GetDriver()->AddBufferToShaderBindings(m_frameDataBinding, "frameData", sizeof(RHI::UboFrameData), 0, RHI::EShaderBindingType::UniformBuffer);

	if (auto textureUID = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<AssetInfoPtr>("Textures/VulkanLogo.png"))
	{
		App::GetSubmodule<TextureImporter>()->LoadTexture(textureUID->GetUID(), defaultTexture)->Then<void, bool>(
			[=](bool bRes) { Sailor::RHI::Renderer::GetDriver()->AddSamplerToShaderBindings(m_frameDataBinding, "g_defaultSampler", defaultTexture->GetRHI(), 1);
		});
	}
	GetWorld()->GetDebugContext()->DrawOrigin(glm::vec4(0, 2, 0, 0), 10.0f, 1000.0f);
}

void TestComponent::EndPlay()
{
}

void TestComponent::Tick(float deltaTime)
{
	auto& transform = GetOwner()->GetTransform();
	const vec3 cameraViewDirection = transform.GetRotation() * Math::vec4_Forward;

	const float sensitivity = 500;

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
		delta += Math::vec3_Forward;

	if (GetWorld()->GetInput().IsKeyDown('Y'))
		delta += Math::vec3_Up;

	if (GetWorld()->GetInput().IsKeyDown('Z'))
		delta += Math::vec3_Right;

	if (GetWorld()->GetInput().IsKeyPressed('F'))
	{
		if (auto camera = GetOwner()->GetComponent<CameraComponent>())
		{
			GetWorld()->GetDebugContext()->DrawFrustum(GetOwner()->GetTransform().GetCachedWorldMatrix(), camera->GetFov(), 20.0f, 1.0f, camera->GetAspect(), glm::vec4(1.0, 1.0, 0.0, 1.0f), 1000.0f);
			//GetWorld()->GetDebugContext()->DrawOrigin(GetOwner()->GetTransform().GetCachedWorldMatrix() * glm::vec4(0, 0, 0, 1.0f), 10.0f, 1000.0f);
		}
	}

	if (glm::length(delta) > 0)
	{
		const vec4 newPosition = transform.GetPosition() + vec4(glm::normalize(delta) * sensitivity * deltaTime, 1.0f);
		transform.SetPosition(newPosition);
	}

	if (GetWorld()->GetInput().IsKeyDown(VK_LBUTTON))
	{
		const float speed = 500.0f;
		const vec2 shift = vec2(GetWorld()->GetInput().GetCursorPos() - m_lastCursorPos) * deltaTime * speed;

		glm::quat hRotation = angleAxis(-glm::radians(shift.x), Math::vec3_Up);
		glm::quat vRotation = angleAxis(glm::radians(shift.y), cross(Math::vec3_Up, cameraViewDirection));
		transform.SetRotation(vRotation * hRotation * transform.GetRotation());
	}

	m_frameData.m_projection = GetOwner()->GetComponent<CameraComponent>()->GetData().GetProjectionMatrix();
	m_frameData.m_currentTime = (float)GetWorld()->GetTime();
	m_frameData.m_deltaTime = deltaTime;

	m_frameData.m_view = glm::lookAt(vec3(transform.GetPosition()),
		vec3(transform.GetPosition()) + transform.GetRotation() * Math::vec3_Forward,
		Math::vec3_Up);

	static float angle = 0;
	//angle += 0.01f;
	glm::mat4x4 model = glm::rotate(glm::mat4(1.0f), glm::radians(angle), Math::vec3_Up);

	SAILOR_PROFILE_BLOCK("Test Performance");

	RHI::Renderer::GetDriverCommands()->UpdateShaderBinding(GetWorld()->GetCommandList(), m_frameDataBinding->GetOrCreateShaderBinding("frameData"), &m_frameData, sizeof(m_frameData));

	if (m_perInstanceData->HasBinding("data"))
	{
		RHI::Renderer::GetDriverCommands()->UpdateShaderBinding(GetWorld()->GetCommandList(), m_perInstanceData->GetOrCreateShaderBinding("data"), &model, sizeof(model));
	}

	auto meshRenderer = GetOwner()->GetComponent<MeshRendererComponent>();
	if (meshRenderer->GetModel() && meshRenderer->GetModel()->IsReady())
	{
		for (auto& material : meshRenderer->GetMaterials())
		{
			// TODO: Move updating shader uniforms to render thread as part of rendering pipeline
			if (material && material->IsReady() && material->GetShaderBindings()->HasBinding("material"))
			{
				RHI::Renderer::GetDriverCommands()->SetMaterialParameter(GetWorld()->GetCommandList(),
					material->GetShaderBindings(),
					"material.color",
					std::max(0.5f, float(sin(0.001 * (double)GetWorld()->GetTime()))) * glm::vec4(1.0, 1.0, 1.0, 1.0f));
			}
		}

		if (m_octree.Num() != meshRenderer->GetModel()->GetMeshes().Num())
		{
			for (auto& mesh : meshRenderer->GetModel()->GetMeshes())
			{
				if (!m_octree.Contains(mesh))
				{
					m_octree.Insert(mesh->m_bounds.GetCenter(), mesh->m_bounds.GetExtents(), mesh);
				}
				//GetWorld()->GetDebugContext()->DrawAABB(mesh->m_bounds);
			}

			if (m_octree.Num() == meshRenderer->GetModel()->GetMeshes().Num())
			{
				//m_octree.DrawOctree(*GetWorld()->GetDebugContext(), 1000);
			}
		}
	}
	/*
	const int count = 73;
	for (int x = -count; x < count; x++)
	{
		for (int y = -count; y < count; y++)
		{
			for (int z = -count; z < count; z++)
			{
				GetWorld()->GetDebugContext()->DrawLine(glm::vec3(x, y, z * 2), glm::vec3(x, y, z * 2 + 1));
			}
		}
	}*/

	SAILOR_PROFILE_END_BLOCK();

	m_lastCursorPos = GetWorld()->GetInput().GetCursorPos();
}


