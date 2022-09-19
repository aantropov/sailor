#include "ECS/LightingECS.h"
#include "ECS/TransformECS.h"
#include "RHI/Shader.h"
#include "Engine/GameObject.h"

using namespace Sailor;
using namespace Sailor::Tasks;

void LightingECS::BeginPlay()
{
	m_lightsData = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();

	Sailor::RHI::Renderer::GetDriver()->AddSsboToShaderBindings(m_lightsData, "light", sizeof(LightingECS::ShaderData), LightsMaxNum, 0);
}

Tasks::ITaskPtr LightingECS::Tick(float deltaTime)
{
	SAILOR_PROFILE_FUNCTION();

	auto renderer = App::GetSubmodule<RHI::Renderer>();
	auto driverCommands = renderer->GetDriverCommands();
	auto cmdList = GetWorld()->GetCommandList();

	for (auto& data : m_components)
	{
		if (!data.m_bIsActive)
			continue;

		const auto& ownerTransform = data.m_owner.StaticCast<GameObject>()->GetTransformComponent();

		if (data.m_bIsDirty || ownerTransform.GetLastFrameChanged() == GetWorld()->GetCurrentFrame())
		{
			size_t index = GetComponentIndex(&data);
			auto& binding = m_lightsData->GetOrAddShaderBinding("light");

			const auto& lightData = m_components[index];

			ShaderData shaderData;
			shaderData.m_attenuation = lightData.m_attenuation;
			shaderData.m_bounds = lightData.m_bounds;
			shaderData.m_intensity = lightData.m_intensity;
			shaderData.m_type = (int32_t)lightData.m_type;
			shaderData.m_direction = ownerTransform.GetForwardVector();
			shaderData.m_worldPosition = ownerTransform.GetWorldPosition();

			RHI::Renderer::GetDriverCommands()->UpdateShaderBinding(cmdList, binding,
				&shaderData,
				sizeof(LightingECS::ShaderData),
				sizeof(LightingECS::ShaderData) * (index + binding->GetStorageInstanceIndex()));

			data.m_bIsDirty = false;
		}
	}

	return nullptr;
}

void LightingECS::EndPlay()
{
	m_lightsData.Clear();
}

void LightingECS::FillLightsData(RHI::RHISceneViewPtr& sceneView)
{
	// TODO: Pass only active lights
	sceneView->m_numLights = (uint32_t)m_components.Num();
	sceneView->m_rhiLightsData = m_lightsData;
}