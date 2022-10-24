#include "ECS/LightingECS.h"
#include "ECS/TransformECS.h"
#include "RHI/Shader.h"
#include "Engine/GameObject.h"

using namespace Sailor;
using namespace Sailor::Tasks;

void LightingECS::BeginPlay()
{
	m_lightsData = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();

	Sailor::RHI::Renderer::GetDriver()->AddSsboToShaderBindings(m_lightsData, "light", sizeof(LightingECS::ShaderData), LightsMaxNum, 0, true);
}

Tasks::ITaskPtr LightingECS::Tick(float deltaTime)
{
	SAILOR_PROFILE_FUNCTION();

	auto renderer = App::GetSubmodule<RHI::Renderer>();
	auto driverCommands = renderer->GetDriverCommands();
	auto cmdList = GetWorld()->GetCommandList();
	auto& binding = m_lightsData->GetOrAddShaderBinding("light");

	TVector<ShaderData> shaderDataBatch;
	shaderDataBatch.Reserve(64);
	bool bShouldWrite = true;
	size_t startIndex = 0;

	uint32_t skipIndex = 0;
	for (size_t index = 0; index < m_components.Num(); index++)
	{
		if (m_skipList.Num() > 0 && skipIndex < m_skipList.Num() && index == m_skipList[skipIndex].m_first)
		{
			index += m_skipList[skipIndex].m_second;
			if (index >= m_components.Num())
			{
				break;
			}
			skipIndex++;
		}

		auto& data = m_components[index];

		if (data.GetOwner()->GetMobilityType() == EMobilityType::Static)
		{
			bool bPlaced = false;

			// Place in the end of the current batch
			if (skipIndex > 0 && m_skipList.Num() > 0 && index == m_skipList[skipIndex - 1].m_first + m_skipList[skipIndex - 1].m_second)
			{
				m_skipList[skipIndex - 1].m_second++;
				bPlaced = true;
			}

			if (!bPlaced && skipIndex > 0 && m_skipList.Num() > 0)
			{
				// Place in between batches
				for (int32_t i = skipIndex - 1; i < (int32_t)m_skipList.Num() - 1; i++)
				{
					const uint32_t start = m_skipList[i].m_first + m_skipList[skipIndex - 1].m_second;
					const uint32_t end = m_skipList[i + 1].m_first;

					if (index > start && index < end)
					{
						m_skipList.Insert(TPair((uint32_t)index, 1u), i + 1);
						bPlaced = true;
						skipIndex++;
						break;
					}
				}
			}

			// Place in the end
			if (!bPlaced)
			{
				m_skipList.Add(TPair((uint32_t)index, 1u));
				skipIndex++;
				bPlaced = true;
			}
		}

		if (!data.m_bIsActive)
			continue;

		const auto& ownerTransform = data.m_owner.StaticCast<GameObject>()->GetTransformComponent();

		if (data.m_bIsDirty || ownerTransform.GetLastFrameChanged() == GetWorld()->GetCurrentFrame())
		{
			if (bShouldWrite)
			{
				bShouldWrite = false;
				startIndex = index;
			}

			const auto& lightData = m_components[index];

			ShaderData shaderData;
			shaderData.m_attenuation = lightData.m_attenuation;
			shaderData.m_bounds = lightData.m_bounds;
			shaderData.m_intensity = lightData.m_intensity;
			shaderData.m_type = (int32_t)lightData.m_type;
			shaderData.m_direction = ownerTransform.GetForwardVector();
			shaderData.m_worldPosition = ownerTransform.GetWorldPosition();

			shaderDataBatch.Emplace(std::move(shaderData));

			data.m_bIsDirty = false;
		}
		else
		{
			bShouldWrite = true;
		}

		if ((bShouldWrite || index == m_components.Num() - 1) && shaderDataBatch.Num() > 0)
		{
			RHI::Renderer::GetDriverCommands()->UpdateShaderBinding(cmdList, binding,
				shaderDataBatch.GetData(),
				sizeof(LightingECS::ShaderData) * shaderDataBatch.Num(),
				binding->GetBufferOffset() +
				sizeof(LightingECS::ShaderData) * startIndex);

			shaderDataBatch.Clear();
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