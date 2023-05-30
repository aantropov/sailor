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

bool CSMLightState::Equals(const CSMLightState& rhs) const
{
	const float CameraPosDelta = 15.0f;
	const float CameraRotationDelta = 0.9995f;

	if (m_componentIndex != rhs.m_componentIndex ||
		m_snapshot.Num() != rhs.m_snapshot.Num() ||
		glm::distance(m_cameraTransform.m_position, rhs.m_cameraTransform.m_position) > CameraPosDelta ||
		glm::dot(m_cameraTransform.GetForward(), rhs.m_cameraTransform.GetForward()) < CameraRotationDelta ||
		m_lightTransform.m_position != rhs.m_lightTransform.m_position ||
		m_lightTransform.m_rotation != rhs.m_lightTransform.m_rotation)
	{
		return false;
	}

	for (uint32_t i = 0; i < m_snapshot.Num(); i++)
	{
		if (m_snapshot[i] != rhs.m_snapshot[i])
		{
			return false;
		}
	}

	return true;
}

void LightingECS::BeginPlay()
{
	auto& driver = Sailor::RHI::Renderer::GetDriver();
	m_lightsData = driver->CreateShaderBindings();
	driver->AddSsboToShaderBindings(m_lightsData, "light", sizeof(LightingECS::LightShaderData), LightsMaxNum, 0, true);

	const auto usage = RHI::ETextureUsageBit::ColorAttachment_Bit |
		RHI::ETextureUsageBit::TextureTransferSrc_Bit |
		RHI::ETextureUsageBit::TextureTransferDst_Bit |
		RHI::ETextureUsageBit::Sampled_Bit;

	m_defaultShadowMap = driver->CreateRenderTarget(glm::ivec2(1, 1), 1, ShadowMapFormat, RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Clamp, usage);

	for (uint32_t i = 0; i < NumCascades; i++)
	{
		char csmDebugName[64];
		sprintf_s(csmDebugName, "Shadow Map, CSM: %d, Cascade: %d", i / NumCascades, i % NumCascades);

		m_csmShadowMaps.Add(driver->CreateRenderTarget(ShadowCascadeResolutions[i % NumCascades], 1,
			ShadowMapFormat_Evsm, RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Clamp, usage));

		driver->SetDebugName(m_csmShadowMaps[m_csmShadowMaps.Num() - 1], csmDebugName);
	}

	TVector<RHI::RHITexturePtr> shadowMaps(MaxShadowsInView);
	for (uint32_t i = 0; i < MaxShadowsInView; i++)
	{
		shadowMaps[i] = (i < m_csmShadowMaps.Num()) ? m_csmShadowMaps[i] : m_defaultShadowMap;
	}

	m_shadowMaps = Sailor::RHI::Renderer::GetDriver()->AddSamplerToShaderBindings(m_lightsData, "shadowMaps", shadowMaps, 8);

	auto shaderBindingSet = m_lightsData;
	m_lightMatrices = Sailor::RHI::Renderer::GetDriver()->AddSsboToShaderBindings(shaderBindingSet, "lightsMatrices", sizeof(glm::mat4), LightingECS::MaxShadowsInView, 6);
	m_shadowIndices = Sailor::RHI::Renderer::GetDriver()->AddSsboToShaderBindings(shaderBindingSet, "shadowIndices", sizeof(uint32_t), LightingECS::MaxShadowsInView, 7);
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
	m_lightMatrices.Clear();
	m_shadowIndices.Clear();
}

void LightingECS::GetLightsInFrustum(const Math::Frustum& frustum,
	const Math::Transform& cameraTransform,
	TVector<RHI::RHILightProxy>& outDirectionalLights,
	TVector<RHI::RHILightProxy>& outSortedPointLights,
	TVector<RHI::RHILightProxy>& outSortedSpotLights)
{
	SAILOR_PROFILE_FUNCTION();
	
	// TODO: Cache lights that cast shadows separately to decrease algo complexity
	for (size_t index = 0; index < m_components.Num(); index++)
	{
		auto& light = m_components[index];
		if (light.m_bCastShadows && light.m_bIsActive)
		{
			const auto& ownerTransform = light.m_owner.StaticCast<GameObject>()->GetTransformComponent();

			RHI::RHILightProxy lightProxy{};

			lightProxy.m_lightMatrix = glm::inverse(ownerTransform.GetCachedWorldMatrix());
			lightProxy.m_lightTransform = ownerTransform.GetTransform();
			lightProxy.m_distanceToCamera = 0.0f;
			lightProxy.m_index = (uint32_t)index;

			if (light.m_type != ELightType::Directional)
			{
				const float sphereRadius = std::max(std::max(light.m_bounds.x, light.m_bounds.y), light.m_bounds.z);

				if (frustum.ContainsSphere(Math::Sphere(ownerTransform.GetPosition(), sphereRadius)))
				{
					// TODO: Sort by screen size, not by distance to camera
					lightProxy.m_distanceToCamera = glm::length(ownerTransform.GetPosition() - cameraTransform.m_position);

					if (ELightType::Spot == light.m_type)
					{
						auto it = std::lower_bound(outSortedSpotLights.begin(), outSortedSpotLights.end(), lightProxy);
						outSortedSpotLights.Insert(lightProxy, it - outSortedSpotLights.begin());
					}
					else
					{
						auto it = std::lower_bound(outSortedPointLights.begin(), outSortedPointLights.end(), lightProxy);
						outSortedPointLights.Insert(lightProxy, it - outSortedPointLights.begin());
					}
				}
			}
			else
			{
				outDirectionalLights.Add(lightProxy);
			}
		}
	}
}

TVector<RHI::RHIUpdateShadowMapCommand> LightingECS::PrepareCSMPasses(
	const RHI::RHISceneViewPtr& sceneView,
	const Math::Transform& cameraTransform,
	const CameraData& cameraData,
	const TVector<RHI::RHILightProxy>& directionalLights)
{
	SAILOR_PROFILE_FUNCTION();

	uint32_t snapshotIndex = 0;
	TVector<RHI::RHIUpdateShadowMapCommand> updateShadowMaps{};
	updateShadowMaps.Reserve(directionalLights.Num() * NumCascades);

	for (const auto& directionalLight : directionalLights)
	{
		auto lightCascadesMatrices = ShadowPrepassNode::CalculateLightProjectionForCascades(directionalLight.m_lightMatrix,
			cameraTransform.Matrix(),
			cameraData.GetAspect(),
			cameraData.GetFov(),
			cameraData.GetZNear(),
			cameraData.GetZFar());

		TVector<Math::Frustum> frustums;
		frustums.Resize(lightCascadesMatrices.Num());

		bool bCascadeAdded[NumCascades] = { false, false, false };
		const uint32_t alreadyPlacedPasses = (uint32_t)updateShadowMaps.Num();

		for (uint32_t k = 0; k < lightCascadesMatrices.Num(); k++)
		{
			auto lightMatrix = lightCascadesMatrices[k] * directionalLight.m_lightMatrix;
			frustums[k].ExtractFrustumPlanes(lightMatrix);

			RHI::RHIUpdateShadowMapCommand cascade;
			cascade.m_meshList = sceneView->TraceScene(frustums[k], true);
			cascade.m_shadowMap = m_csmShadowMaps[k];
			cascade.m_lightMatrix = lightMatrix;
			cascade.m_lighMatrixIndex = k;
			cascade.m_blurRadius = glm::vec2(1, 3);
			
			if (k == 0)
			{
				cascade.m_blurRadius = glm::vec2(2, 5);
			}
			else if (k == 1)
			{
				cascade.m_blurRadius = glm::vec2(1, 3);
			}

			if (k > 0)
			{
				// Don't duplicate data for higher cascades
				cascade.m_meshList.RemoveAll([k, &frustums, bCascadeAdded](const auto& m)
					{
						for (uint32_t z = 0; z < k; z++)
						{
							if (bCascadeAdded[z] && frustums[z].OverlapsAABB(m.m_worldAabb))
							{
								return true;
							}
						}
						return false;
					});

				// We store cascade dependencies
				for (uint32_t z = 0; z < k; z++)
				{
					if (bCascadeAdded[z])
					{
						cascade.m_internalCommandsList.Add(alreadyPlacedPasses + z);
					}
				}
			}

			// Track changes
			CSMLightState snapshot{};
			snapshot.m_componentIndex = directionalLight.m_index;
			snapshot.m_cameraTransform = cameraTransform;
			snapshot.m_lightTransform = directionalLight.m_lightTransform;
			snapshot.m_lightMatrix = lightMatrix;

			for (auto& m : cascade.m_meshList)
			{
				snapshot.m_snapshot.Add({ m.m_staticMeshEcs, m.m_frame });
			}

			if (snapshotIndex < m_csmSnapshots.Num())
			{
				if (snapshot.Equals(m_csmSnapshots[snapshotIndex]))
				{
					snapshotIndex++;
					continue;
				}
				else
				{
					m_csmSnapshots[snapshotIndex] = std::move(snapshot);
				}
			}
			else
			{
				m_csmSnapshots.Emplace(std::move(snapshot));
			}

			bCascadeAdded[k] = true;
			updateShadowMaps.Emplace(cascade);
			snapshotIndex++;
		}
	}

	return updateShadowMaps;
}

void LightingECS::FillLightingData(RHI::RHISceneViewPtr& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	for (uint32_t i = 0; i < sceneView->m_cameraTransforms.Num(); i++)
	{
		const auto& camera = sceneView->m_cameras[i];
		
		Math::Frustum frustum;
		frustum.ExtractFrustumPlanes(sceneView->m_cameraTransforms[i].Matrix(), camera.GetAspect(), camera.GetFov(), camera.GetZNear(), camera.GetZFar());

		// Sort all the lights per camera
		TVector<RHI::RHILightProxy> directionalLights;
		TVector<RHI::RHILightProxy> sortedSpotLights;
		TVector<RHI::RHILightProxy> sortedPointLights;

		GetLightsInFrustum(frustum, sceneView->m_cameraTransforms[i], directionalLights, sortedSpotLights, sortedPointLights);

		// Cache calculated csms to decrease the shadow maps rendering
		const uint32_t numCsmSnapshots = (uint32_t)(sceneView->m_cameraTransforms.Num() * directionalLights.Num() * NumCascades);
		if (m_csmSnapshots.Num() != numCsmSnapshots)
		{
			m_csmSnapshots.Clear();
			m_csmSnapshots.Reserve(numCsmSnapshots);
		}
		
		auto updateShadowMaps = PrepareCSMPasses(sceneView, sceneView->m_cameraTransforms[i], camera, directionalLights);
		sceneView->m_shadowMapsToUpdate.Add(std::move(updateShadowMaps));
	}

	// TODO: Pass only active lights
	sceneView->m_totalNumLights = (uint32_t)m_components.Num();
	sceneView->m_rhiLightsData = m_lightsData;
}

