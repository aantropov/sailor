#include "ShadowPrepassNode.h"
#include "RHI/Batch.hpp"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "RHI/RenderTarget.h"
#include "RHI/Types.h"
#include "RHI/VertexDescription.h"

using namespace Sailor;
using namespace Sailor::RHI;

#ifndef _SAILOR_IMPORT_
const char* ShadowPrepassNode::m_name = "ShadowPrepass";
#endif

RHI::RHIMaterialPtr ShadowPrepassNode::GetOrAddShadowMaterial(RHI::RHIVertexDescriptionPtr vertexDescription)
{
	auto& material = m_shadowMaterials.At_Lock(vertexDescription->GetVertexAttributeBits());

	if (!material)
	{
		auto shaderUID = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ShadowCaster.shader");
		ShaderSetPtr pShader;

		if (App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderUID->GetUID(), pShader))
		{
			RenderState renderState = RHI::RenderState(true, true, 0.0f, false, ECullMode::Back, EBlendMode::None, EFillMode::Fill, GetHash(std::string("Shadow")), false, EDepthCompare::GreaterOrEqual);
			material = RHI::Renderer::GetDriver()->CreateMaterial(vertexDescription, RHI::EPrimitiveTopology::TriangleList, renderState, pShader);
		}
	}
	m_shadowMaterials.Unlock(vertexDescription->GetVertexAttributeBits());

	return material;
}

void ShadowPrepassNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();


	if (sceneView.m_directionalLights.Num() == 0)
	{
		return;
	}

	auto scheduler = App::GetSubmodule<Tasks::Scheduler>();
	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	commands->BeginDebugRegion(commandList, std::string(GetName()), DebugContext::Color_CmdGraphics);

	if (m_csmShadowMaps.Num() == 0)
	{
		const auto usage = RHI::ETextureUsageBit::DepthStencilAttachment_Bit |
			RHI::ETextureUsageBit::TextureTransferSrc_Bit |
			RHI::ETextureUsageBit::TextureTransferDst_Bit |
			RHI::ETextureUsageBit::Sampled_Bit;

		m_defaultShadowMap = driver->CreateRenderTarget(glm::ivec2(1, 1), 1, RHI::EFormat::D16_UNORM, ETextureFiltration::Linear, ETextureClamping::Clamp, usage);

		for (uint32_t i = 0; i < MaxCSM * NumCascades; i++)
		{
			char csmDebugName[64];
			sprintf_s(csmDebugName, "Shadow Map, CSM: %d, Cascade: %d", i / NumCascades, i % NumCascades);

			m_csmShadowMaps.Add(driver->CreateRenderTarget(ShadowCascadeResolutions[i % NumCascades], 1, RHI::EFormat::D16_UNORM, ETextureFiltration::Linear, ETextureClamping::Clamp, usage));
			driver->SetDebugName(m_csmShadowMaps[m_csmShadowMaps.Num() - 1], csmDebugName);
		}

		TVector<RHI::RHITexturePtr> shadowMaps(MaxShadowsInView);
		for (uint32_t i = 0; i < MaxShadowsInView; i++)
		{
			shadowMaps[i] = (i < m_csmShadowMaps.Num()) ? m_csmShadowMaps[i] : m_defaultShadowMap;
		}

		auto shaderBindingSet = sceneView.m_rhiLightsData;
		m_lightMatrices = Sailor::RHI::Renderer::GetDriver()->AddSsboToShaderBindings(shaderBindingSet, "lightsMatrices", sizeof(glm::mat4), ShadowPrepassNode::MaxShadowsInView, 6);
		m_shadowMaps = Sailor::RHI::Renderer::GetDriver()->AddSamplerToShaderBindings(shaderBindingSet, "shadowMaps", shadowMaps, 7);
	}

	const uint32_t NumCSMPasses = (uint32_t)sceneView.m_csmMeshLists.Num() * 3;

	TVector<TDrawCalls<ShadowPrepassNode::PerInstanceData>> drawCalls;
	drawCalls.Resize(NumCSMPasses);

	TVector<TSet<RHIBatch>> batches{ NumCSMPasses };
	batches.Resize(NumCSMPasses);

	uint32_t numMeshes = 0;

	SAILOR_PROFILE_BLOCK("Filter sceneView by tag");

	for (uint32_t lightIndex = 0; lightIndex < sceneView.m_csmMeshLists.Num(); lightIndex++)
	{
		for (uint32_t cascade = 0; cascade < NumCascades; cascade++)
		{
			uint32_t index = lightIndex * NumCascades + cascade;
			for (auto& proxy : sceneView.m_csmMeshLists[lightIndex][cascade])
			{
				uint32_t cascadeNumMeshes = 0;
				for (size_t i = 0; i < proxy.m_meshes.Num(); i++)
				{
					const bool bHasMaterial = proxy.GetMaterials().Num() > i;
					if (!bHasMaterial)
					{
						break;
					}

					const auto& mesh = proxy.m_meshes[i];
					auto depthMaterial = GetOrAddShadowMaterial(mesh->m_vertexDescription);

					if (proxy.GetMaterials()[i]->GetRenderState().IsRequiredCustomDepthShader())
					{
						// TODO: Fix custom depth shader
						// We don't support that yet
						//depthMaterial = proxy.GetMaterials()[i];
						continue;
					}

					const bool bIsDepthMaterialReady = depthMaterial &&
						depthMaterial->GetVertexShader() &&
						depthMaterial->GetFragmentShader() &&
						proxy.GetMaterials()[i]->GetRenderState().IsEnabledZWrite();

					if (!bIsDepthMaterialReady)
					{
						continue;
					}

					ShadowPrepassNode::PerInstanceData data;
					data.model = proxy.m_worldMatrix;

					RHIBatch batch(depthMaterial, mesh);

					drawCalls[index][batch][mesh].Add(data);
					batches[index].Insert(batch);

					numMeshes++;
					cascadeNumMeshes++;
				}
			}

			if (batches[index].Num() == 0)
				continue;

			auto v = *(batches[index].begin());
			size_t a = drawCalls[index][v][v.m_mesh].Num();
			size_t b = drawCalls[index][v].Num();
		}
	}
	SAILOR_PROFILE_END_BLOCK();

	if (numMeshes == 0)
	{
		return;
	}

	SAILOR_PROFILE_BLOCK("Create storage for matrices");

	if (!m_perInstanceData || m_sizePerInstanceData < sizeof(ShadowPrepassNode::PerInstanceData) * numMeshes)
	{
		m_perInstanceData = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
		Sailor::RHI::Renderer::GetDriver()->AddSsboToShaderBindings(m_perInstanceData, "data", sizeof(ShadowPrepassNode::PerInstanceData), numMeshes, 0);
		m_sizePerInstanceData = sizeof(ShadowPrepassNode::PerInstanceData) * numMeshes;
	}

	RHI::RHIShaderBindingPtr storageBinding = m_perInstanceData->GetOrAddShaderBinding("data");
	SAILOR_PROFILE_END_BLOCK();

	TVector<ShadowPrepassNode::PerInstanceData> gpuMatricesData;
	gpuMatricesData.AddDefault(numMeshes);

	SAILOR_PROFILE_BLOCK("Calculate SSBO offsets");
	size_t ssboIndex = 0;
	TVector<TVector<RHIBatch>> passes;
	passes.Resize(NumCSMPasses);

	TVector<TVector<uint32_t>> passIndices;
	passIndices.Resize(NumCSMPasses);

	for (uint32_t i = 0; i < NumCSMPasses; i++)
	{
		auto vecBatches = batches[i].ToVector();
		if (vecBatches.Num() == 0)
		{
			continue;
		}

		TVector<uint32_t> storageIndex(vecBatches.Num());
		for (uint32_t j = 0; j < vecBatches.Num(); j++)
		{
			bool bIsInited = false;
			for (auto& instancedDrawCall : drawCalls[i][vecBatches[j]])
			{
				auto& mesh = instancedDrawCall.First();
				auto& matrices = instancedDrawCall.Second();

				memcpy(&gpuMatricesData[ssboIndex], matrices.GetData(), sizeof(ShadowPrepassNode::PerInstanceData) * matrices.Num());

				if (!bIsInited)
				{
					storageIndex[j] = storageBinding->GetStorageInstanceIndex() + (uint32_t)ssboIndex;
					bIsInited = true;
				}
				ssboIndex += matrices.Num();
			}
		}

		passIndices[i] = std::move(storageIndex);
		passes[i] = std::move(vecBatches);
	}
	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Fill transfer command list with matrices data");
	if (gpuMatricesData.Num() > 0)
	{
		commands->UpdateShaderBinding(transferCommandList, storageBinding,
			gpuMatricesData.GetData(),
			sizeof(ShadowPrepassNode::PerInstanceData) * gpuMatricesData.Num(),
			0);
	}
	SAILOR_PROFILE_END_BLOCK();


	SAILOR_PROFILE_BLOCK("Update directional light matrices");
	TVector<glm::mat4> lightMatrices;
	lightMatrices.Reserve(NumCSMPasses);
	for (uint32_t lightIndex = 0; lightIndex < sceneView.m_csmMeshLists.Num(); lightIndex++)
	{
		auto matrices = CalculateLightProjectionForCascades(sceneView.m_directionalLights[lightIndex].m_lightMatrix,
			sceneView.m_cameraTransform.Matrix(),
			sceneView.m_camera->GetAspect(),
			sceneView.m_camera->GetFov(),
			sceneView.m_camera->GetZNear(),
			sceneView.m_camera->GetZFar());

		for (uint32_t i = 0; i < matrices.Num(); i++)
		{
			Math::Frustum frustum{};
			frustum.ExtractFrustumPlanes(matrices[i] * glm::inverse(sceneView.m_directionalLights[lightIndex].m_lightMatrix));
			lightMatrices.Add(matrices[i] * sceneView.m_directionalLights[lightIndex].m_lightMatrix);
		}
	}

	commands->UpdateShaderBinding(transferCommandList, m_lightMatrices,
		lightMatrices.GetData(),
		sizeof(glm::mat4) * lightMatrices.Num(),
		0);

	SAILOR_PROFILE_END_BLOCK();

	const size_t numThreads = scheduler->GetNumRHIThreads() + 1;
	const size_t materialsPerThread = (batches.Num()) / numThreads;

	if (m_indirectBuffers.Num() < NumCSMPasses)
	{
		m_indirectBuffers.Resize(NumCSMPasses);
	}

	auto shaderBindingsByMaterial = [&](RHIMaterialPtr material)
	{
		TVector<RHIShaderBindingSetPtr> sets({ sceneView.m_frameBindings, m_perInstanceData });
		return sets;
	};

	for (uint32_t lightIndex = 0; lightIndex < sceneView.m_csmMeshLists.Num(); lightIndex++)
	{
		for (uint32_t cascade = 0; cascade < NumCascades; cascade++)
		{
			const uint32_t index = lightIndex * NumCascades + cascade;

			char debugMarker[64];
			sprintf_s(debugMarker, "Record CSM %d, Cascade: %d", lightIndex, cascade);

			SAILOR_PROFILE_BLOCK(debugMarker);

			auto shadowMap = m_csmShadowMaps[cascade];

			commands->BeginDebugRegion(commandList, debugMarker, DebugContext::Color_CmdGraphics);
			{
				commands->BeginRenderPass(commandList,
					TVector<RHI::RHITexturePtr>{},
					shadowMap,
					glm::vec4(0, 0, shadowMap->GetExtent().x, shadowMap->GetExtent().y),
					glm::ivec2(0, 0),
					true,
					glm::vec4(0.0f),
					0.0f,
					false,
					true);

				auto& defaultDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3T3B3UV2C4>();

				commands->PushConstants(commandList, GetOrAddShadowMaterial(defaultDescription), 64, &lightMatrices[index]);

				RHIRecordDrawCall(0, (uint32_t)passes[index].Num(), passes[index], commandList, shaderBindingsByMaterial, drawCalls[index], passIndices[index], m_indirectBuffers[index],
					glm::ivec4(0, shadowMap->GetExtent().y, shadowMap->GetExtent().x, -shadowMap->GetExtent().y),
					glm::uvec4(0, 0, shadowMap->GetExtent().x, shadowMap->GetExtent().y),
					glm::vec2(0.0f, 1.0f));

				for (uint32_t i = 0; i < cascade; i++)
				{
					const uint32_t prevCascadesIndex = lightIndex * NumCascades + i;

					RHIDrawCall(0, (uint32_t)passes[prevCascadesIndex].Num(), passes[prevCascadesIndex], commandList, shaderBindingsByMaterial,
						drawCalls[prevCascadesIndex], m_indirectBuffers[prevCascadesIndex],
						glm::ivec4(0, shadowMap->GetExtent().y, shadowMap->GetExtent().x, -shadowMap->GetExtent().y),
						glm::uvec4(0, 0, shadowMap->GetExtent().x, shadowMap->GetExtent().y),
						glm::vec2(0.0f, 1.0f));
				}

				commands->EndRenderPass(commandList);
			}
			commands->EndDebugRegion(commandList);
			SAILOR_PROFILE_END_BLOCK();
		}
	}
	commands->EndDebugRegion(commandList);
}

void ShadowPrepassNode::Clear()
{
	m_perInstanceData.Clear();
}

glm::mat4 ShadowPrepassNode::CalculateLightProjectionMatrix(const glm::mat4& lightView, const glm::mat4& cameraWorld, float aspect, float fovY, float zNear, float zFar)
{
	SAILOR_PROFILE_FUNCTION();

	Math::Frustum cameraFrustum{};
	cameraFrustum.ExtractFrustumPlanes(cameraWorld, aspect, fovY, zNear, zFar);

	constexpr float zMult = 10.0f;
	return cameraFrustum.CalculateOrthoMatrixByView(lightView, zMult);
}

TVector<glm::mat4> ShadowPrepassNode::CalculateLightProjectionForCascades(const glm::mat4& lightView, const glm::mat4& cameraWorld, float aspect, float fovY, float cameraNearPlane, float cameraFarPlane)
{
	SAILOR_PROFILE_FUNCTION();

	TVector<glm::mat4> ret;
	ret.Add(CalculateLightProjectionMatrix(lightView, cameraWorld, aspect, fovY,
		cameraNearPlane,
		cameraFarPlane * ShadowPrepassNode::ShadowCascadeLevels[0]));

	ret.Add(CalculateLightProjectionMatrix(lightView, cameraWorld, aspect, fovY,
		cameraFarPlane * ShadowPrepassNode::ShadowCascadeLevels[0],
		cameraFarPlane * ShadowPrepassNode::ShadowCascadeLevels[1]));

	ret.Add(CalculateLightProjectionMatrix(lightView, cameraWorld, aspect, fovY,
		cameraFarPlane * ShadowPrepassNode::ShadowCascadeLevels[1],
		cameraFarPlane * ShadowPrepassNode::ShadowCascadeLevels[2]));

	return ret;
}