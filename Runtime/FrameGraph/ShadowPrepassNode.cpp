#include "ShadowPrepassNode.h"
#include "RHI/Batch.hpp"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "RHI/RenderTarget.h"
#include "RHI/Types.h"
#include "RHI/VertexDescription.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "ECS/LightingECS.h"

#include <limits>

using namespace Sailor;
using namespace Sailor::RHI;

#ifndef _SAILOR_IMPORT_
const char* ShadowPrepassNode::m_name = "ShadowPrepass";
#endif

RHI::RHIMaterialPtr ShadowPrepassNode::GetOrAddShadowMaterial(RHI::RHIVertexDescriptionPtr vertexDescription, RHI::EShadowType shadowType, bool bSkinned, bool bMasked)
{
	auto& materials = shadowType == EShadowType::EVSM ?
		(bMasked ?
			(bSkinned ? m_skinnedMaskedShadowMaterials_Evsm : m_maskedShadowMaterials_Evsm) :
			(bSkinned ? m_skinnedShadowMaterials_Evsm : m_shadowMaterials_Evsm)) :
		(bMasked ?
			(bSkinned ? m_skinnedMaskedShadowMaterials_Pcf : m_maskedShadowMaterials_Pcf) :
			(bSkinned ? m_skinnedShadowMaterials_Pcf : m_shadowMaterials_Pcf));
	auto& material = materials[vertexDescription->GetVertexAttributeBits()];

	if (!material)
	{
		auto shaderFileId = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ShadowCaster.shader");
		ShaderSetPtr pShader;

		TVector<std::string> defines;
		if (shadowType == EShadowType::EVSM)
		{
			defines.Add("EVSM");
		}
		if (bSkinned)
		{
			defines.Add("SKINNING");
		}
		if (bMasked)
		{
			defines.Add("MASKED");
		}

		if (App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderFileId->GetFileId(), pShader, defines))
		{
			check(pShader->IsReady());

			const ECullMode cullMode = bMasked ? ECullMode::None : ECullMode::Back;
			RenderState renderState = RHI::RenderState(true, true, 0.0f, false, cullMode, EBlendMode::None, EFillMode::Fill, GetHash(std::string("Shadow")), false, EDepthCompare::GreaterOrEqual);
			material = RHI::Renderer::GetDriver()->CreateMaterial(vertexDescription, RHI::EPrimitiveTopology::TriangleList, renderState, pShader);
		}
	}

	return material;
}

RHI::RHIMaterialPtr ShadowPrepassNode::GetOrAddCustomShadowMaterial(
	const MaterialPtr& sourceMaterial,
	RHI::RHIVertexDescriptionPtr vertexDescription,
	RHI::EShadowType shadowType,
	bool bMasked)
{
	if (!sourceMaterial || !sourceMaterial->GetShader())
	{
		return nullptr;
	}

	auto shaderCompiler = App::GetSubmodule<ShaderCompiler>();
	auto shaderAsset = shaderCompiler->LoadShaderAsset(
		sourceMaterial->GetShader()->GetFileId()).Lock();
	if (!shaderAsset || !shaderAsset->GetSupportedDefines().Contains("SHADOW_CASTER"))
	{
		return nullptr;
	}

	size_t cacheKey = reinterpret_cast<size_t>(sourceMaterial.GetRawPtr());
	HashCombine(cacheKey,
		vertexDescription->GetVertexAttributeBits(),
		static_cast<uint32_t>(shadowType),
		bMasked,
		sourceMaterial->GetContentRevision());
	auto& material = m_customShadowMaterials[cacheKey];
	if (material)
	{
		return material;
	}

	TVector<std::string> defines = sourceMaterial->GetShader()->GetDefines();
	if (!defines.Contains("SHADOW_CASTER"))
	{
		defines.Add("SHADOW_CASTER");
	}
	if (bMasked && shaderAsset->GetSupportedDefines().Contains("ALPHA_CUTOUT") &&
		!defines.Contains("ALPHA_CUTOUT"))
	{
		defines.Add("ALPHA_CUTOUT");
	}
	if (shadowType == EShadowType::EVSM && !defines.Contains("EVSM"))
	{
		defines.Add("EVSM");
	}

	ShaderSetPtr shadowShader;
	if (!shaderCompiler->LoadShader_Immediate(
		sourceMaterial->GetShader()->GetFileId(),
		shadowShader,
		defines) || !shadowShader->IsReady())
	{
		return nullptr;
	}

	const auto& sourceState = sourceMaterial->GetRenderState();
	RenderState shadowState(true,
		true,
		sourceState.GetDepthBias(),
		true,
		sourceState.GetCullMode(),
		EBlendMode::None,
		sourceState.GetFillMode(),
		GetHash(std::string("Shadow")),
		false,
		EDepthCompare::GreaterOrEqual);
	material = RHI::Renderer::GetDriver()->CreateMaterial(
		vertexDescription,
		RHI::EPrimitiveTopology::TriangleList,
		shadowState,
		shadowShader,
		sourceMaterial->GetShaderBindings());
	return material;
}

void ShadowPrepassNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	m_drawCallStats = {};

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	if (!m_pBlurVerticalShader)
	{
		RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
		RenderState renderState{ false, false, 0.0f, false, ECullMode::Front, EBlendMode::None, EFillMode::Fill, 0, false };

		auto shaderFileId = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/Blur.shader");
		if (App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderFileId->GetFileId(), m_pBlurVerticalShader, { "VERTICAL", "EVSM" }))
		{
			m_pBlurVerticalMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pBlurVerticalShader, m_pBlurShaderBindings);
		}

		if (App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderFileId->GetFileId(), m_pBlurHorizontalShader, { "HORIZONTAL", "EVSM" }))
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

	RHIShaderBindingPtr blurDataBinding = m_pBlurShaderBindings->GetOrAddShaderBinding("data");
	auto shaderBindingSet = sceneView.m_rhiLightsData;
	if (shaderBindingSet &&
		shaderBindingSet->HasBinding("shadowIndices") &&
		!sceneView.m_shadowIndices.IsEmpty())
	{
		auto shadowIndices = shaderBindingSet->GetOrAddShaderBinding("shadowIndices");
		if (shadowIndices && shadowIndices->IsBind())
		{
			commands->UpdateShaderBinding(
				transferCommandList,
				shadowIndices,
				sceneView.m_shadowIndices.GetData(),
				sceneView.m_shadowIndices.Num() * sizeof(uint32_t),
				0);
		}
	}
	if (shaderBindingSet &&
		shaderBindingSet->HasBinding("shadowAtlasTiles") &&
		!sceneView.m_shadowAtlasTiles.IsEmpty())
	{
		auto shadowAtlasTiles = shaderBindingSet->GetOrAddShaderBinding("shadowAtlasTiles");
		if (shadowAtlasTiles && shadowAtlasTiles->IsBind())
		{
			commands->UpdateShaderBinding(
				transferCommandList,
				shadowAtlasTiles,
				sceneView.m_shadowAtlasTiles.GetData(),
				sceneView.m_shadowAtlasTiles.Num() * sizeof(uint32_t),
				0);
		}
	}
	if (sceneView.m_shadowMapsToUpdate.Num() == 0)
	{
		return;
	}

	if (!shaderBindingSet || !shaderBindingSet->HasBinding("lightsMatrices"))
	{
		return;
	}

	// A scene/world switch replaces the lighting binding set. Always resolve the
	// target from the current snapshot instead of retaining the previous world.
	m_lightMatrices = shaderBindingSet->GetOrAddShaderBinding("lightsMatrices");
	if (!m_lightMatrices || !m_lightMatrices->IsBind())
	{
		return;
	}

	commands->BeginDebugRegion(commandList, std::string(GetName()), DebugContext::Color_CmdGraphics);
	{
		const uint32_t NumShadowPasses = (uint32_t)sceneView.m_shadowMapsToUpdate.Num();

		TVector<TDrawCalls<ShadowPrepassNode::PerInstanceData>> drawCalls(NumShadowPasses);
		TVector<TSet<RHIBatch>> batches(NumShadowPasses);

		uint32_t numMeshes = 0;
		const size_t opaqueQueueTag = GetHash(std::string("Opaque"));
		const size_t maskedQueueTag = GetHash(std::string("Masked"));
		Framegraph::Details::EvictTextureBindingCache(m_textureBindingCache, sceneView.m_frame);

		SAILOR_PROFILE_SCOPE("Filter sceneView by tag");

		for (uint32_t passIndex = 0; passIndex < sceneView.m_shadowMapsToUpdate.Num(); passIndex++)
		{
			const auto& shadowPass = sceneView.m_shadowMapsToUpdate[passIndex];
			for (const auto& proxy : shadowPass.m_meshList)
			{
				if (!proxy)
				{
					continue;
				}

				for (const auto& shadowMesh : proxy->m_meshes)
				{
					if (!shadowMesh.m_mesh)
					{
						continue;
					}

					if (shadowMesh.m_maxCameraDistance < (std::numeric_limits<float>::max)())
					{
						Math::AABB worldBounds = shadowMesh.m_mesh->m_bounds;
						worldBounds.Apply(shadowMesh.m_worldMatrix);
						const glm::vec3 cameraPosition(sceneView.m_cameraTransform.m_position);
						const glm::vec3 closestPoint = glm::clamp(
							cameraPosition, worldBounds.m_min, worldBounds.m_max);
						if (glm::distance(cameraPosition, closestPoint) > shadowMesh.m_maxCameraDistance)
						{
							continue;
						}
					}
					const size_t renderQueueTag = shadowMesh.m_renderQueueTag;
					if (renderQueueTag != opaqueQueueTag && renderQueueTag != maskedQueueTag)
					{
						continue;
					}

					const auto& mesh = shadowMesh.m_mesh;
					const bool bSkinned =
						proxy->m_skeletonOffset != (std::numeric_limits<uint32_t>::max)() &&
						mesh->m_vertexDescription->HasAttribute(RHI::RHIVertexDescription::DefaultBoneIdsBinding) &&
						mesh->m_vertexDescription->HasAttribute(RHI::RHIVertexDescription::DefaultBoneWeightsBinding);
					const bool bMasked = renderQueueTag == maskedQueueTag;
					auto depthMaterial = GetOrAddShadowMaterial(mesh->m_vertexDescription, shadowPass.m_shadowType, bSkinned, bMasked);
					if (shadowMesh.m_customDepthMaterial)
					{
						auto customShadowMaterial = GetOrAddCustomShadowMaterial(
							shadowMesh.m_customDepthMaterial,
							mesh->m_vertexDescription,
							shadowPass.m_shadowType,
							bMasked);
						if (customShadowMaterial)
						{
							depthMaterial = customShadowMaterial;
						}
					}

					const bool bIsDepthMaterialReady = depthMaterial &&
						depthMaterial->GetVertexShader() &&
						depthMaterial->GetFragmentShader();

					if (!bIsDepthMaterialReady)
					{
						continue;
					}

					ShadowPrepassNode::PerInstanceData data;
					data.model = shadowMesh.m_worldMatrix;
					data.sphereBounds = mesh->m_bounds.ToSphere().GetVec4();
					data.materialInstance = shadowMesh.m_customDepthMaterial &&
						shadowMesh.m_customDepthMaterial->GetShaderBindings() ?
						shadowMesh.m_customDepthMaterial->GetShaderBindings()->GetStorageInstanceIndex("material") :
						0u;
					data.skeletonOffset = bSkinned ? proxy->m_skeletonOffset : (std::numeric_limits<uint32_t>::max)();
					data.baseColorFactor = shadowMesh.m_baseColorFactor;
					data.baseColorSampler = shadowMesh.m_baseColorSampler;
					data.alphaCutoff = shadowMesh.m_alphaCutoff;

					RHIBatch batch(depthMaterial, mesh);
					if (bMasked || depthMaterial->GetRenderState().IsRequiredCustomDepthShader())
					{
						uint32_t supportedMeshesPerBatch = (std::numeric_limits<uint32_t>::max)();
#if defined(__APPLE__)
						batch.m_textureBindings = Framegraph::Details::GetTextureBindingSet(
							m_textureBindingCache,
							shadowMesh.m_materialTextureSamplers,
							sceneView.m_frame,
							supportedMeshesPerBatch);
#else
						batch.m_textureBindings = App::GetSubmodule<TextureImporter>()->GetTextureSamplersBindingSet();
#endif
						batch.m_supportedMeshesPerBatch = supportedMeshesPerBatch;
						if (!batch.m_textureBindings)
						{
							continue;
						}
					}

					drawCalls[passIndex][batch][mesh].Add(data);
					batches[passIndex].Insert(batch);

					numMeshes++;
				}
			}
		}

		TVector<ShadowPrepassNode::PerInstanceData> gpuMatricesData(numMeshes);
		TVector<TVector<RHIBatch>> passes(NumShadowPasses);
		TVector<TVector<uint32_t>> passIndices(NumShadowPasses);
		RHI::RHIShaderBindingPtr storageBinding;

		if (numMeshes > 0)
		{
			if (!m_perInstanceData || m_sizePerInstanceData < sizeof(ShadowPrepassNode::PerInstanceData) * numMeshes)
			{
				SAILOR_PROFILE_SCOPE("Create storage for matrices");
				m_perInstanceData = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
				Sailor::RHI::Renderer::GetDriver()->AddSsboToShaderBindings(m_perInstanceData, "data", sizeof(ShadowPrepassNode::PerInstanceData), numMeshes, 0);
				m_sizePerInstanceData = sizeof(ShadowPrepassNode::PerInstanceData) * numMeshes;
			}

			storageBinding = m_perInstanceData->GetOrAddShaderBinding("data");

			{
				SAILOR_PROFILE_SCOPE("Calculate SSBO offsets");
				size_t ssboIndex = 0;
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
						for (const auto& instancedDrawCall : drawCalls[i][vecBatches[j]])
						{
							auto& matrices = *instancedDrawCall.Second();

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
			}

			SAILOR_PROFILE_SCOPE("Fill transfer command list with matrices data");
			commands->UpdateShaderBinding(transferCommandList, storageBinding,
				gpuMatricesData.GetData(),
				sizeof(ShadowPrepassNode::PerInstanceData) * gpuMatricesData.Num(),
				0);
		}

		if (m_indirectBuffers.Num() < NumShadowPasses)
		{
			m_indirectBuffers.Resize(NumShadowPasses);
		}

		auto shaderBindingsByMaterial = [&](const RHIBatch& batch)
			{
				TVector<RHIShaderBindingSetPtr> sets;
				if (batch.m_material->GetRenderState().IsRequiredCustomDepthShader())
				{
					sets = TVector<RHIShaderBindingSetPtr>({
						sceneView.m_frameBindings,
						sceneView.m_rhiLightsData,
						m_perInstanceData,
						batch.m_material->GetBindings(),
						batch.m_textureBindings });
				}
				else
				{
					sets = TVector<RHIShaderBindingSetPtr>({ sceneView.m_frameBindings, m_perInstanceData });
				}
				if (batch.m_textureBindings)
				{
					if (!batch.m_material->GetRenderState().IsRequiredCustomDepthShader())
					{
						sets.Add(batch.m_textureBindings);
					}
				}
				const bool bSkinned =
					batch.m_mesh->m_vertexDescription->HasAttribute(RHI::RHIVertexDescription::DefaultBoneIdsBinding) &&
					batch.m_mesh->m_vertexDescription->HasAttribute(RHI::RHIVertexDescription::DefaultBoneWeightsBinding);
				if (bSkinned && sceneView.m_boneMatrices)
				{
					sets.Add(sceneView.m_boneMatrices);
				}
				return sets;
			};

		auto fullscreenMesh = frameGraph->GetFullscreenNdcQuad();

		const uint32_t firstIndex = (uint32_t)fullscreenMesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
		const uint32_t vertexOffset = (uint32_t)fullscreenMesh->m_vertexBuffer->GetOffset() / (uint32_t)fullscreenMesh->m_vertexDescription->GetVertexStride();

		for (uint32_t index = 0; index < sceneView.m_shadowMapsToUpdate.Num(); index++)
		{
			char debugMarker[64];
			sprintf_s(debugMarker, sizeof(debugMarker), "Record Shadow Map Pass %d", index);
			SAILOR_PROFILE_SCOPE("Record Shadow Map Pass");

			const auto& shadowPass = sceneView.m_shadowMapsToUpdate[index];
			const glm::ivec2 shadowExtent = shadowPass.m_shadowMap->GetExtent();
			const bool bUsesAtlasTile = shadowPass.m_renderArea.z > 0 && shadowPass.m_renderArea.w > 0;
			const glm::ivec4 renderArea = bUsesAtlasTile ?
				shadowPass.m_renderArea :
				glm::ivec4(0, 0, shadowExtent.x, shadowExtent.y);

			RHI::RHIRenderTargetPtr depthAttachment = driver->GetOrAddTemporaryRenderTarget(
				driver->GetDepthBuffer()->GetFormat(),
				shadowExtent,
				1);

			commands->BeginDebugRegion(commandList, debugMarker, DebugContext::Color_CmdGraphics);
			{
				const auto depthAttachmentLayout = RHI::IsDepthStencilFormat(depthAttachment->GetFormat()) ? EImageLayout::DepthStencilAttachmentOptimal : EImageLayout::DepthAttachmentOptimal;

				commands->ImageMemoryBarrier(commandList, shadowPass.m_shadowMap, EImageLayout::ColorAttachmentOptimal);
				commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachmentLayout);

				// The shadow target stores either raw PCF depth or EVSM moments. With
				// reverse Z, an empty texel represents depth zero. EVSM must encode
				// that value instead of clearing all four moments to zero.
				const glm::vec4 shadowClearValue = shadowPass.m_shadowType == EShadowType::EVSM ?
					glm::vec4(1.0f, 1.0f, -1.0f, 1.0f) :
					glm::vec4(0.0f);

				commands->BeginRenderPass(commandList,
					TVector<RHI::RHITexturePtr>{ shadowPass.m_shadowMap },
					depthAttachment,
					glm::vec4(renderArea),
					glm::ivec2(0, 0),
					!bUsesAtlasTile,
					shadowClearValue,
					0.0f,
					false,
					true);
				if (bUsesAtlasTile)
				{
					commands->ClearAttachments(commandList, renderArea, shadowClearValue, 0.0f);
				}

				RHI::RHIMaterialPtr pushConstantsMaterial;
				if (passes[index].Num() > 0)
				{
					pushConstantsMaterial = passes[index][0].m_material;
				}
				else
				{
					for (uint32_t dependencyPass : shadowPass.m_internalCommandsList)
					{
						if (passes[dependencyPass].Num() > 0)
						{
							pushConstantsMaterial = passes[dependencyPass][0].m_material;
							break;
						}
					}
				}

				if (pushConstantsMaterial)
				{
					commands->PushConstants(commandList, pushConstantsMaterial, 64, &shadowPass.m_lightMatrix);
				}

				if (pushConstantsMaterial && passes[index].Num() > 0)
				{
					m_drawCallStats += RHIRecordDrawCall(0, (uint32_t)passes[index].Num(), passes[index], commandList, transferCommandList, shaderBindingsByMaterial, drawCalls[index], passIndices[index], m_indirectBuffers[index],
						glm::ivec4(renderArea.x, renderArea.y + renderArea.w, renderArea.z, -renderArea.w),
						glm::uvec4(renderArea),
						glm::vec2(0.0f, 1.0f));
				}

				for (uint32_t dependencyPass : shadowPass.m_internalCommandsList)
				{
					const uint32_t numBatches = (uint32_t)passes[dependencyPass].Num();

					if (pushConstantsMaterial && numBatches > 0)
					{
						m_drawCallStats += RHIDrawCall(0, numBatches, passes[dependencyPass], commandList, shaderBindingsByMaterial,
							drawCalls[dependencyPass], m_indirectBuffers[dependencyPass],
							glm::ivec4(renderArea.x, renderArea.y + renderArea.w, renderArea.z, -renderArea.w),
							glm::uvec4(renderArea),
							glm::vec2(0.0f, 1.0f));
					}
				}

				commands->UpdateShaderBinding(transferCommandList, m_lightMatrices,
					&shadowPass.m_lightMatrix,
					sizeof(glm::mat4),
					sizeof(glm::mat4) * shadowPass.m_lighMatrixIndex);

				commands->EndRenderPass(commandList);

				commands->BindVertexBuffer(commandList, fullscreenMesh->m_vertexBuffer, 0);
				commands->BindIndexBuffer(commandList, fullscreenMesh->m_indexBuffer, 0);

				if (shadowPass.m_shadowType == EShadowType::EVSM && shadowPass.m_blurRadius.length() > 0.1f)
				{
					RHI::RHIRenderTargetPtr blurAttachment = driver->GetOrAddTemporaryRenderTarget(shadowPass.m_shadowMap->GetFormat(), shadowPass.m_shadowMap->GetExtent(), 6);
					RHI::Renderer::GetDriverCommands()->UpdateShaderBinding(commandList, blurDataBinding, &shadowPass.m_blurRadius, sizeof(glm::vec2));

					// Blur Horizontal
					commands->BeginDebugRegion(commandList, "Blur Horizontal", DebugContext::Color_CmdPostProcess);
					{
						RHITexturePtr sm = shadowPass.m_shadowMap;
						RHIShaderBindingPtr blurSampler = driver->AddSamplerToShaderBindings(m_pBlurShaderBindings, "colorSampler", { sm }, 1);
						m_pBlurShaderBindings->RecalculateCompatibility();

						commands->ImageMemoryBarrier(commandList, shadowPass.m_shadowMap, EImageLayout::ShaderReadOnlyOptimal);
						commands->ImageMemoryBarrier(commandList, blurAttachment, EImageLayout::ColorAttachmentOptimal);

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
						m_drawCallStats.m_numBatches++;
						m_drawCallStats.m_numInstances++;
						commands->EndRenderPass(commandList);

						commands->ImageMemoryBarrier(commandList, shadowPass.m_shadowMap, EImageLayout::ColorAttachmentOptimal);
						commands->ImageMemoryBarrier(commandList, blurAttachment, EImageLayout::ShaderReadOnlyOptimal);
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
						m_drawCallStats.m_numBatches++;
						m_drawCallStats.m_numInstances++;
						commands->EndRenderPass(commandList);
					}
					commands->EndDebugRegion(commandList);

					driver->ReleaseTemporaryRenderTarget(blurAttachment);
				}

				// Lighting samples every completed shadow map later in the same
				// graphics command list. Publish the color writes explicitly for both
				// the direct PCF path and the final EVSM blur pass.
				commands->ImageMemoryBarrier(commandList, shadowPass.m_shadowMap, EImageLayout::ShaderReadOnlyOptimal);

				driver->ReleaseTemporaryRenderTarget(depthAttachment);

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
	m_shadowMaterials_Pcf.Clear();
	m_shadowMaterials_Evsm.Clear();
	m_skinnedShadowMaterials_Pcf.Clear();
	m_skinnedShadowMaterials_Evsm.Clear();
	m_maskedShadowMaterials_Pcf.Clear();
	m_maskedShadowMaterials_Evsm.Clear();
	m_skinnedMaskedShadowMaterials_Pcf.Clear();
	m_skinnedMaskedShadowMaterials_Evsm.Clear();
	m_customShadowMaterials.Clear();
	m_textureBindingCache.Clear();
}

glm::mat4 ShadowPrepassNode::CalculateLightProjectionMatrix(const glm::mat4& lightView, const glm::mat4& cameraWorld, float aspect, float fovY, float zNear, float zFar, float zMult, glm::ivec2 shadowMapResolution, float zSourceExtension)
{
	SAILOR_PROFILE_FUNCTION();

	Math::Frustum cameraFrustum{};
	cameraFrustum.ExtractFrustumPlanes(cameraWorld, aspect, fovY, zNear, zFar);
	return cameraFrustum.CalculateOrthoMatrixByView(lightView, zMult, shadowMapResolution, zSourceExtension);
}

TVector<glm::mat4> ShadowPrepassNode::CalculateLightProjectionForCascades(const glm::mat4& lightView, const glm::mat4& cameraWorld, float aspect, float fovY, float cameraNearPlane, float cameraFarPlane)
{
	SAILOR_PROFILE_FUNCTION();
	const float shadowFarPlane = (std::min)(cameraFarPlane, LightingECS::ShadowMaxDistance);

	TVector<glm::mat4> ret;
	for (uint32_t i = 0; i < LightingECS::NumCascades; ++i)
	{
		const float cascadeFar = shadowFarPlane * LightingECS::ShadowCascadeLevels[i];
		float cascadeNear = cameraNearPlane;
		if (i > 0)
		{
			const float previousSplit = shadowFarPlane * LightingECS::ShadowCascadeLevels[i - 1];
			const float previousNear = i > 1 ?
				shadowFarPlane * LightingECS::ShadowCascadeLevels[i - 2] : cameraNearPlane;
			const float overlap = (previousSplit - previousNear) * LightingECS::ShadowCascadeBlendFraction;
			cascadeNear = (std::max)(cameraNearPlane, previousSplit - overlap);
		}

		ret.Add(CalculateLightProjectionMatrix(lightView, cameraWorld, aspect, fovY,
			cascadeNear,
			cascadeFar,
			10.0f,
			LightingECS::ShadowCascadeResolutions[i],
			LightingECS::ShadowCasterDepthExtension));
	}

	return ret;
}
