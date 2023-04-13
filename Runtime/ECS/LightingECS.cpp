#include "ECS/LightingECS.h"
#include "ECS/TransformECS.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "RHI/DebugContext.h"
#include "Engine/GameObject.h"
#include "FrameGraph/ShadowPrepassNode.h"

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

	driverCommands->BeginDebugRegion(cmdList, "LightingECS:Update Lights", RHI::DebugContext::Color_CmdTransfer);

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
			shaderData.m_cutOff = vec2(glm::cos(glm::radians(lightData.m_cutOff.x)), glm::cos(glm::radians(lightData.m_cutOff.y)));
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

	driverCommands->EndDebugRegion(cmdList);

	return nullptr;
}

void LightingECS::EndPlay()
{
	m_lightsData.Clear();
}

void LightingECS::FillLightsData(RHI::RHISceneViewPtr& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	TVector<Math::Frustum> frustums;
	frustums.Reserve(sceneView->m_cameraTransforms.Num());

	for (uint32_t j = 0; j < sceneView->m_cameraTransforms.Num(); j++)
	{
		const auto& camera = sceneView->m_cameras[j];

		Math::Frustum frustum;
		frustum.ExtractFrustumPlanes(sceneView->m_cameraTransforms[j].Matrix(), camera.GetAspect(), camera.GetFov(), camera.GetZNear(), camera.GetZFar());
		frustums.Emplace(std::move(frustum));
	}

	sceneView->m_sortedSpotLights.Resize(sceneView->m_cameraTransforms.Num());
	sceneView->m_sortedPointLights.Resize(sceneView->m_cameraTransforms.Num());

	// TODO: Cache lights that cast shadows separately to decrease algo complexity
	for (size_t index = 0; index < m_components.Num(); index++)
	{
		auto& light = m_components[index];
		if (light.m_bCastShadows && light.m_bIsActive)
		{
			const auto& ownerTransform = light.m_owner.StaticCast<GameObject>()->GetTransformComponent();

			RHI::RHILightProxy lightProxy{};

			lightProxy.m_lightMatrix = glm::inverse(ownerTransform.GetCachedWorldMatrix());
			lightProxy.m_shadowMap = nullptr;
			lightProxy.m_lastShadowMapUpdate = 0;
			lightProxy.m_distanceToCamera = 0.0f;
			lightProxy.m_index = (uint32_t)index;

			if (light.m_type != ELightType::Directional)
			{

				for (uint32_t j = 0; j < sceneView->m_cameraTransforms.Num(); j++)
				{
					const float sphereRadius = std::max(std::max(light.m_bounds.x, light.m_bounds.y), light.m_bounds.z);

					if (frustums[j].ContainsSphere(Math::Sphere(ownerTransform.GetPosition(), sphereRadius)))
					{
						// TODO: Sort by screen size, not by distance to camera
						lightProxy.m_distanceToCamera = glm::length(ownerTransform.GetPosition() - sceneView->m_cameraTransforms[j].m_position);

						if (ELightType::Spot == light.m_type)
						{
							auto it = std::lower_bound(sceneView->m_sortedSpotLights[j].begin(), sceneView->m_sortedSpotLights[j].end(), lightProxy);
							sceneView->m_sortedSpotLights[j].Insert(lightProxy, it - sceneView->m_sortedSpotLights[j].begin());
						}
						else
						{
							auto it = std::lower_bound(sceneView->m_sortedPointLights[j].begin(), sceneView->m_sortedPointLights[j].end(), lightProxy);
							sceneView->m_sortedPointLights[j].Insert(lightProxy, it - sceneView->m_sortedPointLights[j].begin());
						}
					}
				}
			}
			else
			{
				sceneView->m_directionalLights.Add(lightProxy);
			}
		}
	}

	sceneView->m_csmMeshLists.Resize(sceneView->m_cameraTransforms.Num());

	for (uint32_t i = 0; i < sceneView->m_cameraTransforms.Num(); i++)
	{
		sceneView->m_csmMeshLists[i].Resize(sceneView->m_directionalLights.Num());

		for (uint32_t j = 0; j < sceneView->m_directionalLights.Num(); j++)
		{
			sceneView->m_csmMeshLists[i][j].Resize(ShadowPrepassNode::NumCascades);

			auto lightCascadesMatrices = ShadowPrepassNode::CalculateLightProjectionForCascades(sceneView->m_directionalLights[j].m_lightMatrix,
				sceneView->m_cameraTransforms[i].Matrix(),
				sceneView->m_cameras[i].GetAspect(),
				sceneView->m_cameras[i].GetFov(),
				sceneView->m_cameras[i].GetZNear(),
				sceneView->m_cameras[i].GetZFar());

			for (uint32_t k = 0; k < lightCascadesMatrices.Num(); k++)
			{
				Math::Frustum frustum{};
				frustum.ExtractFrustumPlanes(lightCascadesMatrices[k] * sceneView->m_directionalLights[j].m_lightMatrix);
				sceneView->m_stationaryOctree.Trace(frustum, sceneView->m_csmMeshLists[i][j][k]);
			}
		}
	}
	// TODO: Pass only active lights
	sceneView->m_totalNumLights = (uint32_t)m_components.Num();
	sceneView->m_rhiLightsData = m_lightsData;
}

