#include "ECS/LightingECS.h"
#include "ECS/TransformECS.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "RHI/RenderTarget.h"
#include "RHI/SceneView.h"
#include "RHI/DebugContext.h"
#include "Engine/GameObject.h"
#include "FrameGraph/ShadowPrepassNode.h"

using namespace Sailor;
using namespace Sailor::Tasks;

void LightingECS::BeginPlay()
{
	auto& driver = Sailor::RHI::Renderer::GetDriver();
	m_lightsData = driver->CreateShaderBindings();
	driver->AddSsboToShaderBindings(m_lightsData, "light", sizeof(LightingECS::LightShaderData), LightsMaxNum, 0, true);

	if (m_csmShadowMaps.Num() == 0)
	{
		const auto usage = RHI::ETextureUsageBit::DepthStencilAttachment_Bit |
			RHI::ETextureUsageBit::TextureTransferSrc_Bit |
			RHI::ETextureUsageBit::TextureTransferDst_Bit |
			RHI::ETextureUsageBit::Sampled_Bit;

		m_defaultShadowMap = driver->CreateRenderTarget(glm::ivec2(1, 1), 1, ShadowMapFormat, RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Clamp, usage);

		for (uint32_t i = 0; i < NumCascades; i++)
		{
			char csmDebugName[64];
			sprintf_s(csmDebugName, "Shadow Map, CSM: %d, Cascade: %d", i / NumCascades, i % NumCascades);

			m_csmShadowMaps.Add(driver->CreateRenderTarget(ShadowCascadeResolutions[i % NumCascades], 1,
				ShadowMapFormat, RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Clamp, usage));
			
			driver->SetDebugName(m_csmShadowMaps[m_csmShadowMaps.Num() - 1], csmDebugName);
		}

		TVector<RHI::RHITexturePtr> shadowMaps(MaxShadowsInView);
		for (uint32_t i = 0; i < MaxShadowsInView; i++)
		{
			shadowMaps[i] = (i < m_csmShadowMaps.Num()) ? m_csmShadowMaps[i] : m_defaultShadowMap;
		}

		m_shadowMaps = Sailor::RHI::Renderer::GetDriver()->AddSamplerToShaderBindings(m_lightsData, "shadowMaps", shadowMaps, 7);		
	}
}

Tasks::ITaskPtr LightingECS::Tick(float deltaTime)
{
	SAILOR_PROFILE_FUNCTION();

	auto renderer = App::GetSubmodule<RHI::Renderer>();
	auto driverCommands = renderer->GetDriverCommands();
	auto cmdList = GetWorld()->GetCommandList();
	auto& binding = m_lightsData->GetOrAddShaderBinding("light");

	driverCommands->BeginDebugRegion(cmdList, "LightingECS:Update Lights", RHI::DebugContext::Color_CmdTransfer);

	TVector<LightShaderData> shaderDataBatch;
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
		auto owner = data.m_owner.StaticCast<GameObject>();

		if (owner->GetMobilityType() == EMobilityType::Static)
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

		const auto& ownerTransform = owner->GetTransformComponent();

		if (data.m_bIsDirty || data.m_frameLastChange < owner->GetFrameLastChange())
		{
			if (bShouldWrite)
			{
				bShouldWrite = false;
				startIndex = index;
			}

			const auto& lightData = m_components[index];

			LightShaderData shaderData;
			shaderData.m_attenuation = lightData.m_attenuation;
			shaderData.m_bounds = lightData.m_bounds;
			shaderData.m_intensity = lightData.m_intensity;
			shaderData.m_type = (int32_t)lightData.m_type;
			shaderData.m_direction = ownerTransform.GetForwardVector();
			shaderData.m_worldPosition = ownerTransform.GetWorldPosition();
			shaderData.m_cutOff = vec2(glm::cos(glm::radians(lightData.m_cutOff.x)), glm::cos(glm::radians(lightData.m_cutOff.y)));
			shaderDataBatch.Emplace(std::move(shaderData));

			data.m_frameLastChange = GetWorld()->GetCurrentFrame();
			
			if (data.m_frameLastChange != owner->GetFrameLastChange())
			{
				UpdateGameObject(owner, GetWorld()->GetCurrentFrame());
			}

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
				sizeof(LightingECS::LightShaderData) * shaderDataBatch.Num(),
				binding->GetBufferOffset() +
				sizeof(LightingECS::LightShaderData) * startIndex);

			shaderDataBatch.Clear();
		}
	}

	driverCommands->EndDebugRegion(cmdList);

	return nullptr;
}

void LightingECS::EndPlay()
{
	m_lightsData.Clear();
	m_csmShadowMaps.Clear();
	m_defaultShadowMap.Clear();
	m_shadowMaps.Clear();
}

void LightingECS::FillLightingData(RHI::RHISceneViewPtr& sceneView)
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

	// Sort all the lights per camera
	TVector<RHI::RHILightProxy> directionalLights;
	TVector<TVector<RHI::RHILightProxy>> sortedSpotLights{ sceneView->m_cameraTransforms.Num() };
	TVector<TVector<RHI::RHILightProxy>> sortedPointLights{ sceneView->m_cameraTransforms.Num() };

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
							auto it = std::lower_bound(sortedSpotLights[j].begin(), sortedSpotLights[j].end(), lightProxy);
							sortedSpotLights[j].Insert(lightProxy, it - sortedSpotLights[j].begin());
						}
						else
						{
							auto it = std::lower_bound(sortedPointLights[j].begin(), sortedPointLights[j].end(), lightProxy);
							sortedPointLights[j].Insert(lightProxy, it - sortedPointLights[j].begin());
						}
					}
				}
			}
			else
			{
				directionalLights.Add(lightProxy);
			}
		}
	}

	for (uint32_t i = 0; i < sceneView->m_cameraTransforms.Num(); i++)
	{
		TVector<RHI::RHIUpdateShadowMapCommand> updateShadowMaps;
		
		for (const auto& directionalLight : directionalLights)
		{
			auto lightCascadesMatrices = ShadowPrepassNode::CalculateLightProjectionForCascades(directionalLight.m_lightMatrix,
				sceneView->m_cameraTransforms[i].Matrix(),
				sceneView->m_cameras[i].GetAspect(),
				sceneView->m_cameras[i].GetFov(),
				sceneView->m_cameras[i].GetZNear(),
				sceneView->m_cameras[i].GetZFar());

			TVector<Math::Frustum> frustums;
			frustums.Resize(lightCascadesMatrices.Num());

			for (uint32_t k = 0; k < lightCascadesMatrices.Num(); k++)
			{
				uint32_t cascadeIndex = (uint32_t)updateShadowMaps.Add(RHI::RHIUpdateShadowMapCommand());
				auto& cascade = updateShadowMaps[cascadeIndex];

				auto lightMatrix = lightCascadesMatrices[k] * directionalLight.m_lightMatrix;

				frustums[k].ExtractFrustumPlanes(lightMatrix);
				cascade.m_meshList = sceneView->TraceScene(frustums[k], true);

				// TODO:Redo
				cascade.m_shadowMap = m_csmShadowMaps[k];
				cascade.m_lightMatrix = lightMatrix;
				if (k > 0)
				{
					// Don't duplicate data for higher cascades
					cascade.m_meshList.RemoveAll([k = k, &frustums](const auto& m)
						{
							for (uint32_t z = 0; z < k; z++)
							{
								if (frustums[z].OverlapsAABB(m.m_worldAabb))
								{
									return true;
								}
							}
							return false;
						});

					// We store cascade dependencies
					for (int32_t z = k; z > 0; z--)
					{
						cascade.m_internalCommandsList.Add(cascadeIndex - z);
					}
				}
			}
		}

		sceneView->m_shadowMapsToUpdate.Emplace(std::move(updateShadowMaps));
	}

	// TODO: Pass only active lights
	sceneView->m_totalNumLights = (uint32_t)m_components.Num();
	sceneView->m_rhiLightsData = m_lightsData;
}

