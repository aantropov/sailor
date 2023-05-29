#include "ShadowPrepassNode.h"
#include "RHI/Batch.hpp"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "RHI/RenderTarget.h"
#include "RHI/Types.h"
#include "RHI/VertexDescription.h"

#include "ECS/LightingECS.h"

using namespace Sailor;
using namespace Sailor::RHI;

#ifndef _SAILOR_IMPORT_
const char* ShadowPrepassNode::m_name = "ShadowPrepass";
#endif

RHI::RHIMaterialPtr ShadowPrepassNode::GetOrAddShadowMaterial(RHI::RHIVertexDescriptionPtr vertexDescription)
{
	auto& material = m_shadowMaterials[vertexDescription->GetVertexAttributeBits()];

	if (!material)
	{
		auto shaderUID = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ShadowCaster.shader");
		ShaderSetPtr pShader;

		if (App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderUID->GetUID(), pShader, { "EVSM" }))
		{
			check(pShader->IsReady());

			RenderState renderState = RHI::RenderState(true, true, 0.0f, false, ECullMode::Back, EBlendMode::None, EFillMode::Fill, GetHash(std::string("Shadow")), false, EDepthCompare::GreaterOrEqual);
			material = RHI::Renderer::GetDriver()->CreateMaterial(vertexDescription, RHI::EPrimitiveTopology::TriangleList, renderState, pShader);
		}
	}

	return material;
}

void ShadowPrepassNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto scheduler = App::GetSubmodule<Tasks::Scheduler>();
	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	if (!m_pBlurVerticalShader)
	{
		RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
		RenderState renderState{ false, false, 0.0f, false, ECullMode::Front, EBlendMode::None, EFillMode::Fill, 0, false };

		auto shaderUID = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/Blur.shader");
		if (App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderUID->GetUID(), m_pBlurVerticalShader, { "VERTICAL" }))
		{
			m_pBlurVerticalMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pBlurVerticalShader, m_pBlurShaderBindings);
		}

		if (App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderUID->GetUID(), m_pBlurHorizontalShader, { "HORIZONTAL" }))
		{
			m_pBlurHorizontalMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pBlurHorizontalShader, m_pBlurShaderBindings);
		}

		m_pBlurShaderBindings = driver->CreateShaderBindings();
		driver->FillShadersLayout(m_pBlurShaderBindings, { m_pBlurVerticalShader->GetDebugVertexShaderRHI(), m_pBlurVerticalShader->GetDebugFragmentShaderRHI() }, 1);

		RHIShaderBindingPtr blurDataBinding = driver->AddBufferToShaderBindings(m_pBlurShaderBindings, "data", sizeof(glm::vec4) * 3, 0, RHI::EShaderBindingType::UniformBuffer);
		const float defaultBlurRadius = 3.0f;
		glm::vec4 blurData[] = { {defaultBlurRadius, 0, 0, 0}, {0,0,0,0}, {0,0,0,0} };
		RHI::Renderer::GetDriverCommands()->UpdateShaderBinding(transferCommandList, blurDataBinding, &blurData, sizeof(glm::vec4) * 3);
	}

	RHIShaderBindingPtr blurDataBinding = driver->AddBufferToShaderBindings(m_pBlurShaderBindings, "data", sizeof(glm::vec4) * 3, 0, RHI::EShaderBindingType::UniformBuffer);

	if (!m_lightMatrices)
	{
		auto shaderBindingSet = sceneView.m_rhiLightsData;
		m_lightMatrices = shaderBindingSet->GetOrAddShaderBinding("lightsMatrices");
	}

	if (sceneView.m_shadowMapsToUpdate.Num() == 0)
	{
		return;
	}

	commands->BeginDebugRegion(commandList, std::string(GetName()), DebugContext::Color_CmdGraphics);
	{
		const uint32_t NumShadowPasses = (uint32_t)sceneView.m_shadowMapsToUpdate.Num();

		TVector<TDrawCalls<ShadowPrepassNode::PerInstanceData>> drawCalls;
		drawCalls.Resize(NumShadowPasses);

		TVector<TSet<RHIBatch>> batches{ NumShadowPasses };
		batches.Resize(NumShadowPasses);

		uint32_t numMeshes = 0;

		SAILOR_PROFILE_BLOCK("Filter sceneView by tag");

		for (uint32_t passIndex = 0; passIndex < sceneView.m_shadowMapsToUpdate.Num(); passIndex++)
		{
			const auto& shadowPass = sceneView.m_shadowMapsToUpdate[passIndex];
			for (auto& proxy : shadowPass.m_meshList)
			{
				for (size_t i = 0; i < proxy.m_meshes.Num(); i++)
				{
					if (!proxy.m_bCastShadows)
					{
						continue;
					}

					const auto& mesh = proxy.m_meshes[i];
					auto depthMaterial = GetOrAddShadowMaterial(mesh->m_vertexDescription);

					const bool bIsDepthMaterialReady = depthMaterial &&
						depthMaterial->GetVertexShader() &&
						depthMaterial->GetFragmentShader();

					if (!bIsDepthMaterialReady)
					{
						continue;
					}

					ShadowPrepassNode::PerInstanceData data;
					data.model = proxy.m_worldMatrix;

					RHIBatch batch(depthMaterial, mesh);

					drawCalls[passIndex][batch][mesh].Add(data);
					batches[passIndex].Insert(batch);

					numMeshes++;
				}
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
		passes.Resize(NumShadowPasses);

		TVector<TVector<uint32_t>> passIndices;
		passIndices.Resize(NumShadowPasses);

		for (uint32_t i = 0; i < NumShadowPasses; i++)
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

		const size_t numThreads = scheduler->GetNumRHIThreads() + 1;
		const size_t materialsPerThread = (batches.Num()) / numThreads;

		if (m_indirectBuffers.Num() < NumShadowPasses)
		{
			m_indirectBuffers.Resize(NumShadowPasses);
		}

		auto shaderBindingsByMaterial = [&](RHIMaterialPtr material)
		{
			TVector<RHIShaderBindingSetPtr> sets({ sceneView.m_frameBindings, m_perInstanceData });
			return sets;
		};

		auto fullscreenMesh = frameGraph->GetFullscreenNdcQuad();

		const uint32_t firstIndex = (uint32_t)fullscreenMesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
		const uint32_t vertexOffset = (uint32_t)fullscreenMesh->m_vertexBuffer->GetOffset() / (uint32_t)fullscreenMesh->m_vertexDescription->GetVertexStride();

		for (uint32_t index = 0; index < sceneView.m_shadowMapsToUpdate.Num(); index++)
		{
			char debugMarker[64];
			sprintf_s(debugMarker, "Record Shadow Map Pass %d", index);
			SAILOR_PROFILE_BLOCK(debugMarker);

			const auto& shadowPass = sceneView.m_shadowMapsToUpdate[index];

			RHI::RHIRenderTargetPtr depthAttachment = driver->GetOrAddTemporaryRenderTarget(driver->GetDepthBuffer()->GetFormat(), shadowPass.m_shadowMap->GetExtent(), 1);
			
			commands->BeginDebugRegion(commandList, debugMarker, DebugContext::Color_CmdGraphics);
			{
				commands->ImageMemoryBarrier(commandList, shadowPass.m_shadowMap, shadowPass.m_shadowMap->GetFormat(), shadowPass.m_shadowMap->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);
				commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), depthAttachment->GetDefaultLayout(), EImageLayout::DepthAttachmentOptimal);

				commands->BeginRenderPass(commandList,
					TVector<RHI::RHITexturePtr>{ shadowPass.m_shadowMap },
					depthAttachment,
					glm::vec4(0, 0, shadowPass.m_shadowMap->GetExtent().x, shadowPass.m_shadowMap->GetExtent().y),
					glm::ivec2(0, 0),
					true,
					glm::vec4(0.0f),
					0.0f,
					false,
					true);

				auto& defaultDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3T3B3UV2C4>();

				commands->PushConstants(commandList, GetOrAddShadowMaterial(defaultDescription), 64, &sceneView.m_shadowMapsToUpdate[index].m_lightMatrix);

				RHIRecordDrawCall(0, (uint32_t)passes[index].Num(), passes[index], commandList, shaderBindingsByMaterial, drawCalls[index], passIndices[index], m_indirectBuffers[index],
					glm::ivec4(0, shadowPass.m_shadowMap->GetExtent().y, shadowPass.m_shadowMap->GetExtent().x, -shadowPass.m_shadowMap->GetExtent().y),
					glm::uvec4(0, 0, shadowPass.m_shadowMap->GetExtent().x, shadowPass.m_shadowMap->GetExtent().y),
					glm::vec2(0.0f, 1.0f));

				for (auto& dependencyPass : shadowPass.m_internalCommandsList)
				{
					RHIDrawCall(0, (uint32_t)passes[dependencyPass].Num(), passes[dependencyPass], commandList, shaderBindingsByMaterial,
						drawCalls[dependencyPass], m_indirectBuffers[dependencyPass],
						glm::ivec4(0, shadowPass.m_shadowMap->GetExtent().y, shadowPass.m_shadowMap->GetExtent().x, -shadowPass.m_shadowMap->GetExtent().y),
						glm::uvec4(0, 0, shadowPass.m_shadowMap->GetExtent().x, shadowPass.m_shadowMap->GetExtent().y),
						glm::vec2(0.0f, 1.0f));
				}

				commands->UpdateShaderBinding(transferCommandList, m_lightMatrices,
					&shadowPass.m_lightMatrix,
					sizeof(glm::mat4),
					sizeof(glm::mat4) * shadowPass.m_lighMatrixIndex);

				commands->EndRenderPass(commandList);
				commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), EImageLayout::DepthAttachmentOptimal, depthAttachment->GetDefaultLayout());

				commands->BindVertexBuffer(commandList, fullscreenMesh->m_vertexBuffer, 0);
				commands->BindIndexBuffer(commandList, fullscreenMesh->m_indexBuffer, 0);

				if (shadowPass.m_blurRadius > 0)
				{
					RHI::RHIRenderTargetPtr blurAttachment = driver->GetOrAddTemporaryRenderTarget(shadowPass.m_shadowMap->GetFormat(), shadowPass.m_shadowMap->GetExtent(), 6);

					const float blurFloat = (float)shadowPass.m_blurRadius;
					RHI::Renderer::GetDriverCommands()->UpdateShaderBinding(commandList, blurDataBinding, &blurFloat, sizeof(float));

					// Blur Horizontal
					commands->BeginDebugRegion(commandList, "Blur Horizontal", DebugContext::Color_CmdPostProcess);
					{
						RHITexturePtr sm = shadowPass.m_shadowMap;
						RHIShaderBindingPtr blurSampler = driver->AddSamplerToShaderBindings(m_pBlurShaderBindings, "colorSampler", { sm }, 1);
						m_pBlurShaderBindings->RecalculateCompatibility();

						commands->ImageMemoryBarrier(commandList, shadowPass.m_shadowMap, shadowPass.m_shadowMap->GetFormat(), EImageLayout::ColorAttachmentOptimal, EImageLayout::ShaderReadOnlyOptimal);
						commands->ImageMemoryBarrier(commandList, blurAttachment, blurAttachment->GetFormat(), blurAttachment->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);

						commands->BeginRenderPass(commandList,
							TVector<RHI::RHITexturePtr>{blurAttachment},
							nullptr,
							glm::vec4(0, 0, shadowPass.m_shadowMap->GetExtent().x, shadowPass.m_shadowMap->GetExtent().y),
							glm::ivec2(0, 0),
							false,
							glm::vec4(0.0f),
							0.0f,
							false);

						commands->BindMaterial(commandList, m_pBlurHorizontalMaterial);
						commands->BindShaderBindings(commandList, m_pBlurHorizontalMaterial, { sceneView.m_frameBindings, m_pBlurShaderBindings });

						commands->SetViewport(commandList,
							0, 0,
							(float)shadowPass.m_shadowMap->GetExtent().x, (float)shadowPass.m_shadowMap->GetExtent().y,
							glm::vec2(0, 0),
							glm::vec2(shadowPass.m_shadowMap->GetExtent().x, shadowPass.m_shadowMap->GetExtent().y),
							0, 1.0f);

						commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
						commands->EndRenderPass(commandList);

						commands->ImageMemoryBarrier(commandList, shadowPass.m_shadowMap, shadowPass.m_shadowMap->GetFormat(), EImageLayout::ShaderReadOnlyOptimal, EImageLayout::ColorAttachmentOptimal);
						commands->ImageMemoryBarrier(commandList, blurAttachment, blurAttachment->GetFormat(), EImageLayout::ColorAttachmentOptimal, EImageLayout::ShaderReadOnlyOptimal);
					}
					commands->EndDebugRegion(commandList);

					// Blur Vertical
					commands->BeginDebugRegion(commandList, "Blur Vertical", DebugContext::Color_CmdPostProcess);
					{
						RHIShaderBindingPtr blurSampler = driver->AddSamplerToShaderBindings(m_pBlurShaderBindings, "colorSampler", TVector<RHITexturePtr>{ blurAttachment }, 1);
						m_pBlurShaderBindings->RecalculateCompatibility();

						commands->BeginRenderPass(commandList,
							TVector<RHI::RHITexturePtr>{shadowPass.m_shadowMap},
							nullptr,
							glm::vec4(0, 0, shadowPass.m_shadowMap->GetExtent().x, shadowPass.m_shadowMap->GetExtent().y),
							glm::ivec2(0, 0),
							false,
							glm::vec4(0.0f),
							0.0f,
							false);

						commands->BindMaterial(commandList, m_pBlurVerticalMaterial);
						commands->BindShaderBindings(commandList, m_pBlurVerticalMaterial, { sceneView.m_frameBindings, m_pBlurShaderBindings });

						commands->SetViewport(commandList,
							0, 0,
							(float)shadowPass.m_shadowMap->GetExtent().x, (float)shadowPass.m_shadowMap->GetExtent().y,
							glm::vec2(0, 0),
							glm::vec2(shadowPass.m_shadowMap->GetExtent().x, shadowPass.m_shadowMap->GetExtent().y),
							0, 1.0f);

						commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
						commands->EndRenderPass(commandList);

						commands->ImageMemoryBarrier(commandList, shadowPass.m_shadowMap, shadowPass.m_shadowMap->GetFormat(), EImageLayout::ColorAttachmentOptimal, shadowPass.m_shadowMap->GetDefaultLayout());
						commands->ImageMemoryBarrier(commandList, blurAttachment, blurAttachment->GetFormat(), EImageLayout::ShaderReadOnlyOptimal, blurAttachment->GetDefaultLayout());
					}
					commands->EndDebugRegion(commandList);

					driver->ReleaseTemporaryRenderTarget(blurAttachment);
				}
				else
				{
					commands->ImageMemoryBarrier(commandList, shadowPass.m_shadowMap, shadowPass.m_shadowMap->GetFormat(), EImageLayout::ColorAttachmentOptimal, shadowPass.m_shadowMap->GetDefaultLayout());
				}

				driver->ReleaseTemporaryRenderTarget(depthAttachment);

				SAILOR_PROFILE_END_BLOCK();
			}
			commands->EndDebugRegion(commandList);

		}
	}
	commands->EndDebugRegion(commandList);
}

void ShadowPrepassNode::Clear()
{
	m_lightMatrices.Clear();
	m_perInstanceData.Clear();
}

glm::mat4 ShadowPrepassNode::CalculateLightProjectionMatrix(const glm::mat4& lightView, const glm::mat4& cameraWorld, float aspect, float fovY, float zNear, float zFar, float zMult)
{
	SAILOR_PROFILE_FUNCTION();

	Math::Frustum cameraFrustum{};
	cameraFrustum.ExtractFrustumPlanes(cameraWorld, aspect, fovY, zNear, zFar);
	return cameraFrustum.CalculateOrthoMatrixByView(lightView, zMult);
}

TVector<glm::mat4> ShadowPrepassNode::CalculateLightProjectionForCascades(const glm::mat4& lightView, const glm::mat4& cameraWorld, float aspect, float fovY, float cameraNearPlane, float cameraFarPlane)
{
	SAILOR_PROFILE_FUNCTION();

	TVector<glm::mat4> ret;
	ret.Add(CalculateLightProjectionMatrix(lightView, cameraWorld, aspect, fovY,
		cameraNearPlane,
		cameraFarPlane * LightingECS::ShadowCascadeLevels[0], 20.0f));

	ret.Add(CalculateLightProjectionMatrix(lightView, cameraWorld, aspect, fovY,
		cameraFarPlane * LightingECS::ShadowCascadeLevels[0],
		cameraFarPlane * LightingECS::ShadowCascadeLevels[1], 10.0f));

	ret.Add(CalculateLightProjectionMatrix(lightView, cameraWorld, aspect, fovY,
		cameraFarPlane * LightingECS::ShadowCascadeLevels[1],
		cameraFarPlane * LightingECS::ShadowCascadeLevels[2], 5.0f));

	return ret;
}