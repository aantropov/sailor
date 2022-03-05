#include "Components/TestComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Engine/GameObject.h"
#include "Engine/EngineLoop.h"

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
}

void TestComponent::EndPlay()
{
}

void TestComponent::Tick(float deltaTime)
{
	static glm::vec3 cameraPosition = Math::vec3_Forward * -10.0f;
	static glm::vec3 cameraViewDir = Math::vec3_Forward;

	const float sensitivity = 500;

	glm::vec3 delta = glm::vec3(0.0f, 0.0f, 0.0f);
	if (GetWorld()->GetInput().IsKeyDown('A'))
		delta += -cross(cameraViewDir, Math::vec3_Up);

	if (GetWorld()->GetInput().IsKeyDown('D'))
		delta += cross(cameraViewDir, Math::vec3_Up);

	if (GetWorld()->GetInput().IsKeyDown('W'))
		delta += cameraViewDir;

	if (GetWorld()->GetInput().IsKeyDown('S'))
		delta += -cameraViewDir;

	if (glm::length(delta) > 0)
		cameraPosition += glm::normalize(delta) * sensitivity * deltaTime;

	if (GetWorld()->GetInput().IsKeyDown(VK_LBUTTON))
	{
		const float speed = 500.0f;
		const vec2 shift = vec2(GetWorld()->GetInput().GetCursorPos() - m_lastCursorPos) * deltaTime * speed;

		glm::quat hRotation = angleAxis(-glm::radians(shift.x), Math::vec3_Up);
		glm::quat vRotation = angleAxis(glm::radians(shift.y), cross(Math::vec3_Up, cameraViewDir));

		cameraViewDir = vRotation * cameraViewDir;
		cameraViewDir = hRotation * cameraViewDir;
	}

	auto width = Sailor::App::GetViewportWindow()->GetWidth();
	auto height = Sailor::App::GetViewportWindow()->GetHeight();

	float aspect = (height + width) > 0 ? width / (float)height : 1.0f;
	m_frameData.m_projection = glm::perspective(glm::radians(90.0f), aspect, 0.1f, 10000.0f);
	m_frameData.m_projection[1][1] *= -1;
	m_frameData.m_currentTime = (float)GetWorld()->GetTime();
	m_frameData.m_deltaTime = deltaTime;
	m_frameData.m_view = glm::lookAt(cameraPosition, cameraPosition + cameraViewDir, Math::vec3_Up);

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
			if (material && material->IsReady() && material->GetRHI()->GetBindings()->HasBinding("material"))
			{
				RHI::Renderer::GetDriverCommands()->SetMaterialParameter(GetWorld()->GetCommandList(),
					material->GetRHI(),
					"material.color",
					std::max(0.5f, float(sin(0.001 * (double)GetWorld()->GetTime()))) * glm::vec4(1.0, 1.0, 1.0, 1.0f));
			}
		}
	}

	SAILOR_PROFILE_END_BLOCK();

	m_lastCursorPos = GetWorld()->GetInput().GetCursorPos();
}


