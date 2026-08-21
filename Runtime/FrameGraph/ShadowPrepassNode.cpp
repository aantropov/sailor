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
	const ShaderSetPtr& sourceShader,
	const RHI::RHIMaterialPtr& sourceMaterial,
	const RHI::RHIMaterialVersionPtr& sourceMaterialVersion,
	RHI::RHIVertexDescriptionPtr vertexDescription,
	RHI::EShadowType shadowType,
	bool bMasked)
{
	if (!sourceShader || !sourceMaterial ||
		!sourceMaterialVersion || !sourceMaterialVersion->GetBindings())
	{
		return nullptr;
	}

	auto shaderCompiler = App::GetSubmodule<ShaderCompiler>();
	auto shaderAsset = shaderCompiler->LoadShaderAsset(
		sourceShader->GetFileId()).Lock();
	if (!shaderAsset || !shaderAsset->GetSupportedDefines().Contains("PACKED_SHADOW_CASTER"))
	{
		return nullptr;
	}

	size_t cacheKey = reinterpret_cast<size_t>(sourceMaterial.GetRawPtr());
	HashCombine(cacheKey,
		vertexDescription->GetVertexAttributeBits(),
		static_cast<uint32_t>(shadowType),
		bMasked);
	auto& entry = m_customShadowMaterials[cacheKey];
	if (entry.m_sourceVersion == sourceMaterialVersion && entry.m_material)
	{
		return entry.m_material;
	}

	TVector<std::string> defines = sourceShader->GetDefines();
	if (!defines.Contains("PACKED_SHADOW_CASTER"))
	{
		defines.Add("PACKED_SHADOW_CASTER");
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
		sourceShader->GetFileId(),
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
	auto material = RHI::Renderer::GetDriver()->CreateMaterial(
		vertexDescription,
		RHI::EPrimitiveTopology::TriangleList,
		shadowState,
		shadowShader,
		sourceMaterialVersion->GetBindings());
	if (material)
	{
		entry.m_sourceVersion = sourceMaterialVersion;
		entry.m_material = material;
	}
	return material;
}

void ShadowPrepassNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	m_drawCallStats = {};
	if (!sceneView.m_submissionContext)
	{
		return;
	}
	const uint64_t materialSubmissionId =
		sceneView.m_submissionContext->GetSubmissionId();

	auto submissionResources = sceneView.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(
		this,
		sceneView.m_cameraIndex,
		0u);
	auto& blurShaderBindings = submissionResources->m_blurShaderBindings;
	auto& renderPassColorAttachments =
		submissionResources->m_renderPassColorAttachments;
	auto& blurDrawBindingSets = submissionResources->m_blurDrawBindingSets;

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	std::string virtualizeInstancePayloadsSetting;
	const bool bVirtualizeInstancePayloads =
		!TryGetString("VirtualizeInstancePayloads", virtualizeInstancePayloadsSetting) ||
		virtualizeInstancePayloadsSetting != "false";

	if (!m_pBlurVerticalShader)
	{
		RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
		RenderState renderState{ false, false, 0.0f, false, ECullMode::Front, EBlendMode::None, EFillMode::Fill, 0, false };

		auto shaderFileId = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/Blur.shader");
		const bool bVerticalReady = App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderFileId->GetFileId(), m_pBlurVerticalShader, { "VERTICAL", "EVSM" });
		const bool bHorizontalReady = App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderFileId->GetFileId(), m_pBlurHorizontalShader, { "HORIZONTAL", "EVSM" });

		m_pBlurShaderBindings = driver->CreateShaderBindings();
		driver->FillShadersLayout(m_pBlurShaderBindings, { m_pBlurVerticalShader->GetDebugVertexShaderRHI(), m_pBlurVerticalShader->GetDebugFragmentShaderRHI() }, 1);
		driver->AddBufferToShaderBindings(m_pBlurShaderBindings, "data", sizeof(glm::vec4) * 3, 0, RHI::EShaderBindingType::UniformBuffer);

		if (bVerticalReady)
		{
			m_pBlurVerticalMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pBlurVerticalShader, m_pBlurShaderBindings);
		}
		if (bHorizontalReady)
		{
			m_pBlurHorizontalMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pBlurHorizontalShader, m_pBlurShaderBindings);
		}
	}

	if (!blurShaderBindings)
	{
		blurShaderBindings = driver->CreateShaderBindings();
		driver->FillShadersLayout(blurShaderBindings, { m_pBlurVerticalShader->GetDebugVertexShaderRHI(), m_pBlurVerticalShader->GetDebugFragmentShaderRHI() }, 1);
		RHIShaderBindingPtr initialBlurDataBinding = driver->AddBufferToShaderBindings(blurShaderBindings, "data", sizeof(glm::vec4) * 3, 0, RHI::EShaderBindingType::UniformBuffer);
		const float defaultBlurRadius = 3.0f;
		glm::vec4 blurData[] = { {defaultBlurRadius, 0, 0, 0}, {0,0,0,0}, {0,0,0,0} };
		RHI::Renderer::GetDriverCommands()->UpdateShaderBinding(transferCommandList, initialBlurDataBinding, &blurData, sizeof(glm::vec4) * 3);
	}

	RHIShaderBindingPtr blurDataBinding = blurShaderBindings->GetOrAddShaderBinding("data");

	if (!sceneView.m_shadowMapsToBlit.IsEmpty())
	{
		commands->BeginDebugRegion(
			commandList,
			"Downsample Local Shadow Tiles",
			DebugContext::Color_CmdTransfer);
		for (const auto& blit : sceneView.m_shadowMapsToBlit)
		{
			if (!blit.m_source || !blit.m_destination ||
				blit.m_sourceArea.z <= 0 || blit.m_sourceArea.w <= 0 ||
				blit.m_destinationArea.z <= 0 || blit.m_destinationArea.w <= 0)
			{
				continue;
			}

			if (blit.m_source == blit.m_destination)
			{
				auto scratch = driver->GetOrAddTemporaryRenderTarget(
					blit.m_source->GetFormat(),
					glm::ivec2(blit.m_destinationArea.z, blit.m_destinationArea.w),
					1);
				commands->ImageMemoryBarrier(
					commandList, blit.m_source, EImageLayout::TransferSrcOptimal);
				commands->ImageMemoryBarrier(
					commandList, scratch, EImageLayout::TransferDstOptimal);
				commands->BlitImage(
					commandList,
					blit.m_source,
					scratch,
					blit.m_sourceArea,
					glm::ivec4(0, 0, scratch->GetExtent().x, scratch->GetExtent().y),
					ETextureFiltration::Nearest);

				commands->ImageMemoryBarrier(
					commandList, scratch, EImageLayout::TransferSrcOptimal);
				commands->ImageMemoryBarrier(
					commandList, blit.m_destination, EImageLayout::TransferDstOptimal);
				commands->BlitImage(
					commandList,
					scratch,
					blit.m_destination,
					glm::ivec4(0, 0, scratch->GetExtent().x, scratch->GetExtent().y),
					blit.m_destinationArea,
					ETextureFiltration::Nearest);
				commands->ImageMemoryBarrier(
					commandList, scratch, EImageLayout::ShaderReadOnlyOptimal);
				driver->ReleaseTemporaryRenderTarget(scratch);
			}
			else
			{
				commands->ImageMemoryBarrier(
					commandList, blit.m_source, EImageLayout::TransferSrcOptimal);
				commands->ImageMemoryBarrier(
					commandList, blit.m_destination, EImageLayout::TransferDstOptimal);
				commands->BlitImage(
					commandList,
					blit.m_source,
					blit.m_destination,
					blit.m_sourceArea,
					blit.m_destinationArea,
					ETextureFiltration::Nearest);
			}

			commands->ImageMemoryBarrier(
				commandList, blit.m_source, EImageLayout::ShaderReadOnlyOptimal);
			commands->ImageMemoryBarrier(
				commandList, blit.m_destination, EImageLayout::ShaderReadOnlyOptimal);
		}
		commands->EndDebugRegion(commandList);
	}
	if (sceneView.m_shadowMapsToUpdate.Num() == 0)
	{
		return;
	}

	commands->BeginDebugRegion(commandList, std::string(GetName()), DebugContext::Color_CmdGraphics);
	{
		const uint32_t NumShadowPasses = (uint32_t)sceneView.m_shadowMapsToUpdate.Num();
		const size_t opaqueQueueTag = GetHash(std::string("Opaque"));
		const size_t maskedQueueTag = GetHash(std::string("Masked"));
		const size_t staticPayloadIndex =
			RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(
				EMobilityType::Static);
		const size_t stationaryPayloadIndex =
			RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(
				EMobilityType::Stationary);
		const bool bUsesPagedArenas = bVirtualizeInstancePayloads;
		auto usesPagedArena = [&](size_t payloadIndex)
			{
				return bUsesPagedArenas &&
					(payloadIndex == staticPayloadIndex ||
						payloadIndex == stationaryPayloadIndex);
			};
		auto buildPayloadCacheSlot = [&](
			uint64_t viewKey,
			EShadowType shadowType,
			EMobilityType mobility)
			{
				const size_t index =
					RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(mobility);
				size_t cacheSlot = usesPagedArena(index) ?
					1469598103934665603ull : viewKey;
				if (usesPagedArena(index))
				{
					HashCombine(cacheSlot, static_cast<uint32_t>(shadowType), index);
				}
				else
				{
					HashCombine(cacheSlot, sceneView.m_cameraIndex, index);
				}
				return cacheSlot;
			};
		submissionResources->m_activeShadowViews.Clear(false);
		submissionResources->m_activeShadowViews.Reserve(NumShadowPasses);
		submissionResources->m_numActiveShadowViews = NumShadowPasses;
		auto& shadowPayloadRevisions = submissionResources->m_shadowPayloadRevisions;
		auto& bBuildShadowPayloads = submissionResources->m_buildShadowPayloads;
		auto& bShadowPayloadComplete = submissionResources->m_shadowPayloadComplete;
		auto& requestedPacketTextures =
			submissionResources->m_requestedPacketTextures;
		shadowPayloadRevisions.Clear(false);
		bBuildShadowPayloads.Clear(false);
		bShadowPayloadComplete.Clear(false);
		shadowPayloadRevisions.Resize(NumShadowPasses);
		bBuildShadowPayloads.Resize(NumShadowPasses);
		bShadowPayloadComplete.Resize(NumShadowPasses);
		for (uint32_t passIndex = 0u; passIndex < NumShadowPasses; ++passIndex)
		{
			for (auto& requestedTextures : requestedPacketTextures)
			{
				requestedTextures.Reset();
			}
			const auto& shadowPass = sceneView.m_shadowMapsToUpdate[passIndex];
			size_t viewKey = 1469598103934665603ull;
			HashCombine(
				viewKey,
				shadowPass.m_lighMatrixIndex,
				static_cast<uint32_t>(shadowPass.m_shadowType));
			auto& viewResources = submissionResources->m_shadowViewCache[viewKey];
			if (!viewResources)
			{
				viewResources = TSharedPtr<SubmissionResources::ShadowViewResources>::Make();
			}
			viewResources->Begin(viewKey);
			submissionResources->m_activeShadowViews.Add(viewResources);

			std::array<uint32_t, RHI::TPackedDrawPacket<PerInstanceData>::NumMobilitySegments>
				numRelevantCasters{};
			for (size_t index = 0u; index < shadowPayloadRevisions[passIndex].size(); ++index)
			{
				auto& revision = shadowPayloadRevisions[passIndex][index];
				revision = 1469598103934665603ull;
				HashCombine(
					revision,
					index,
					shadowPass.m_lighMatrixIndex,
					static_cast<uint32_t>(shadowPass.m_shadowType),
					std::hash<glm::mat4>{}(shadowPass.m_lightMatrix),
					std::hash<glm::vec3>{}(glm::vec3(sceneView.m_cameraTransform.m_position)));
				bBuildShadowPayloads[passIndex][index] = true;
				bShadowPayloadComplete[passIndex][index] = true;
			}
			if (bUsesPagedArenas)
			{
				for (const EMobilityType mobility :
					{ EMobilityType::Static, EMobilityType::Stationary })
				{
					const size_t payloadIndex =
						RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(mobility);
					auto& arenaRevision =
						shadowPayloadRevisions[passIndex][payloadIndex];
					arenaRevision = 1469598103934665603ull;
					HashCombine(
						arenaRevision,
						payloadIndex,
						static_cast<uint32_t>(shadowPass.m_shadowType),
						sceneView.m_submissionContext->GetMaterialRevision(),
						sceneView.GetMobilityRevision(mobility));
				}
			}

			for (const auto& proxy : shadowPass.m_meshList)
			{
				const auto* source = proxy.GetSource();
				if (!source || !proxy.m_resource)
				{
					continue;
				}
				const EMobilityType mobility = proxy.GetMobility();
				const size_t payloadIndex =
					RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(mobility);
				auto& revision = shadowPayloadRevisions[passIndex][payloadIndex];
				++numRelevantCasters[payloadIndex];
				if (!usesPagedArena(payloadIndex))
				{
					HashCombine(
						revision,
						proxy.m_handle.m_slot,
						proxy.m_handle.m_generation,
						proxy.m_resource->m_shadowRevision,
						proxy.GetProducerKey(),
						std::hash<glm::mat4>{}(proxy.GetWorldMatrix()),
						proxy.GetSkeletonOffset());
				}

				for (const auto& shadowMesh : source->m_meshes)
				{
					if (shadowMesh.m_renderQueueTag != opaqueQueueTag &&
						shadowMesh.m_renderQueueTag != maskedQueueTag)
					{
						continue;
					}
					if (!usesPagedArena(payloadIndex))
					{
						HashCombine(revision, proxy.ResolveMesh(shadowMesh, shadowPass.m_lightMatrix));
					}
#if defined(__APPLE__)
					if (shadowMesh.m_renderQueueTag == maskedQueueTag ||
						shadowMesh.m_customDepthMaterial)
					{
						for (uint32_t texture : shadowMesh.m_materialTextureSamplers)
						{
							requestedPacketTextures[payloadIndex].Insert(texture);
						}
					}
#endif
				}
				const auto* topology = &proxy.m_resource->m_proxy;
				for (const auto& group : topology->m_instancedGroups)
				{
					if (!group.m_bCastShadows)
					{
						continue;
					}
					for (size_t meshIndex = 0u; meshIndex < group.m_meshes.Num(); ++meshIndex)
					{
						const size_t renderQueueTag = meshIndex < group.m_renderQueueTags.Num() ?
							group.m_renderQueueTags[meshIndex] : 0u;
						if (renderQueueTag != opaqueQueueTag && renderQueueTag != maskedQueueTag)
						{
							continue;
						}
						if (!usesPagedArena(payloadIndex))
						{
							HashCombine(
								revision,
								proxy.ResolveMesh(group.m_meshes[meshIndex], shadowPass.m_lightMatrix));
						}
#if defined(__APPLE__)
						const bool bCustomDepth = meshIndex < group.m_materials.Num() &&
							group.m_materials[meshIndex] &&
							group.m_materials[meshIndex]->GetRenderState().IsRequiredCustomDepthShader();
						if ((renderQueueTag == maskedQueueTag || bCustomDepth) &&
							meshIndex < group.m_materialTextureSamplers.Num())
						{
							for (uint32_t texture : group.m_materialTextureSamplers[meshIndex])
							{
								requestedPacketTextures[payloadIndex].Insert(texture);
							}
						}
#endif
					}
				}
			}

			for (size_t index = 0u; index < shadowPayloadRevisions[passIndex].size(); ++index)
			{
				if (!usesPagedArena(index))
				{
					HashCombine(
						shadowPayloadRevisions[passIndex][index],
						numRelevantCasters[index],
						Framegraph::Details::CalculateTextureDependencyRevision(
							requestedPacketTextures[index].GetIndices()));
				}
			}
			if (bVirtualizeInstancePayloads)
			{
				for (const EMobilityType mobility :
					{ EMobilityType::Static, EMobilityType::Stationary })
				{
					const size_t index =
						RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(mobility);
					const size_t cacheSlot = buildPayloadCacheSlot(
						viewKey,
						shadowPass.m_shadowType,
						mobility);
					auto payload = usesPagedArena(index) ?
						m_pagedArenaCache.Find(
							cacheSlot,
							shadowPayloadRevisions[passIndex][index],
							sceneView.m_frame) :
						m_packetPayloadCache.Find(
							cacheSlot,
							shadowPayloadRevisions[passIndex][index],
							sceneView.m_frame);
					if (payload)
					{
						if (usesPagedArena(index))
						{
							viewResources->m_packet.UseSharedArenaPayload(
								mobility,
								std::move(payload));
						}
						else
						{
							viewResources->m_packet.UseSharedPayload(
								mobility,
								std::move(payload));
						}
						bBuildShadowPayloads[passIndex][index] = false;
					}
				}
			}
		}

		Framegraph::Details::EvictTextureBindingCache(m_textureBindingCache, sceneView.m_frame);
		m_packetPayloadCache.Evict(sceneView.m_frame);

		SAILOR_PROFILE_SCOPE("Filter sceneView by tag");

		for (uint32_t passIndex = 0; passIndex < sceneView.m_shadowMapsToUpdate.Num(); passIndex++)
		{
			const auto& shadowPass = sceneView.m_shadowMapsToUpdate[passIndex];
			auto& viewResources =
				*submissionResources->m_activeShadowViews[passIndex];
			if (bUsesPagedArenas)
			{
				for (const EMobilityType mobility : {EMobilityType::Static, EMobilityType::Stationary})
				{
					const size_t arenaPayloadIndex = RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(mobility);
					if (!bBuildShadowPayloads[passIndex][arenaPayloadIndex])
					{
						continue;
					}
					const size_t arenaCacheSlot =
						buildPayloadCacheSlot(viewResources.m_viewKey, shadowPass.m_shadowType, mobility);
					if (auto payload = m_pagedArenaCache.Find(
							arenaCacheSlot, shadowPayloadRevisions[passIndex][arenaPayloadIndex], sceneView.m_frame))
					{
						viewResources.m_packet.UseSharedArenaPayload(mobility, std::move(payload));
						bBuildShadowPayloads[passIndex][arenaPayloadIndex] = false;
					}
					else
					{
						m_pagedArenaCache.BeginUpdate(
							arenaCacheSlot, shadowPayloadRevisions[passIndex][arenaPayloadIndex], sceneView.m_frame);
						auto& rangeInstances = submissionResources->m_arenaRangeInstances;
						auto& rangeStableKeys = submissionResources->m_arenaRangeStableKeys;
						auto& rangeMaterialVersionRuns = submissionResources->m_arenaRangeMaterialVersionRuns;
						sceneView.ForEachShadowCaster(mobility,
							[&](const RHIVisibleShadowCaster& proxy)
							{
								const auto* source = proxy.GetSource();
								if (!source || !proxy.m_resource)
								{
									return;
								}
								const uint64_t rangeKey =
									BuildPackedDrawRangeKey(proxy.m_handle, proxy.GetProducerKey(), proxy.m_resource);
								size_t rangeRevision = proxy.m_resource->m_shadowRevision;
								HashCombine(rangeRevision, static_cast<uint32_t>(shadowPass.m_shadowType),
									proxy.GetContentRevision(), std::hash<glm::mat4>{}(proxy.GetWorldMatrix()),
									proxy.GetSkeletonOffset());
								if (proxy.m_record)
								{
									HashCombine(rangeRevision, proxy.m_record->m_materialRevision,
										proxy.m_record->m_shadowRevision, proxy.m_record->m_renderFlags);
								}
								for (const auto& shadowMesh : source->m_meshes)
								{
									if (shadowMesh.m_customDepthMaterial)
									{
										const auto materialVersion =
											shadowMesh.m_customDepthMaterial->GetVersionForSubmission(
												materialSubmissionId);
										HashCombine(
											rangeRevision, materialVersion ? materialVersion->GetVersionId() : 0ull);
									}
								}
								for (const auto& group : proxy.m_resource->m_proxy.m_instancedGroups)
								{
									for (const auto& material : group.m_materials)
									{
										if (material && material->GetRenderState().IsRequiredCustomDepthShader())
										{
											const auto materialVersion =
												material->GetVersionForSubmission(materialSubmissionId);
											HashCombine(rangeRevision,
												materialVersion ? materialVersion->GetVersionId() : 0ull);
										}
									}
								}
								if (m_pagedArenaCache.TryReuseRange(rangeKey, rangeRevision))
								{
									return;
								}
								rangeInstances.Clear(false);
								rangeStableKeys.Clear(false);
								rangeMaterialVersionRuns.Clear(false);

								auto addArenaShadowInstance =
									[&](const RHIMeshPtr& mesh, const glm::mat4& model, size_t renderQueueTag,
										float baseColorAlpha, uint32_t baseColorSampler, float alphaCutoff,
										const ShaderSetPtr& customDepthShader,
										const RHIMaterialPtr& customDepthMaterial,
										const RHIMaterialVersionPtr& customDepthMaterialVersion, uint64_t stableKey)
								{
									if (renderQueueTag != opaqueQueueTag && renderQueueTag != maskedQueueTag)
									{
										return;
									}
									if (!mesh)
									{
										bShadowPayloadComplete[passIndex][arenaPayloadIndex] = false;
										return;
									}

									const bool bSkinned =
										proxy.GetSkeletonOffset() != (std::numeric_limits<uint32_t>::max)() &&
										mesh->m_vertexDescription->HasAttribute(
											RHIVertexDescription::DefaultBoneIdsBinding) &&
										mesh->m_vertexDescription->HasAttribute(
											RHIVertexDescription::DefaultBoneWeightsBinding);
									const bool bMasked = renderQueueTag == maskedQueueTag;
									auto depthMaterial = GetOrAddShadowMaterial(
										mesh->m_vertexDescription, shadowPass.m_shadowType, bSkinned, bMasked);
									if (customDepthMaterial)
									{
										if (auto customShadowMaterial = GetOrAddCustomShadowMaterial(customDepthShader,
												customDepthMaterial, customDepthMaterialVersion,
												mesh->m_vertexDescription, shadowPass.m_shadowType, bMasked))
										{
											depthMaterial = customShadowMaterial;
										}
									}
									if (!depthMaterial || !depthMaterial->GetVertexShader() ||
										!depthMaterial->GetFragmentShader())
									{
										bShadowPayloadComplete[passIndex][arenaPayloadIndex] = false;
										return;
									}

									// Shadow materials are immutable derivatives of the exact source
									// version resolved for this submission.
									RHIBatch batch(depthMaterial, mesh);
									const auto materialBindings = batch.GetMaterialBindings();
									PerInstanceData data;
									data.model = model;
									data.sphereBounds = mesh->m_bounds.ToSphere().GetVec4();
									data.materialInstance =
										materialBindings ? materialBindings->GetStorageInstanceIndex("material") : 0u;
									data.skeletonOffset =
										bSkinned ? proxy.GetSkeletonOffset() : (std::numeric_limits<uint32_t>::max)();
									data.bakedVolumeScale = vec4(mesh->m_bakedVolumeScale, 1.0f);
									data.baseColorAlpha = baseColorAlpha;
									data.baseColorSampler = baseColorSampler;
									data.alphaCutoff = alphaCutoff;
									rangeInstances.Add(std::move(data));
									rangeStableKeys.Add(stableKey);
									AppendPackedDrawArenaMaterialVersion(
										rangeMaterialVersionRuns, batch.m_materialVersion);
								};

								for (size_t shadowMeshIndex = 0u; shadowMeshIndex < source->m_meshes.Num();
									++shadowMeshIndex)
								{
									const auto& shadowMesh = source->m_meshes[shadowMeshIndex];
									addArenaShadowInstance(shadowMesh.m_mesh, proxy.ResolveMeshWorldMatrix(shadowMesh),
										shadowMesh.m_renderQueueTag, shadowMesh.m_baseColorFactor.a,
										shadowMesh.m_baseColorSampler, shadowMesh.m_alphaCutoff,
										shadowMesh.m_customDepthShader, shadowMesh.m_customDepthMaterial,
										shadowMesh.m_customDepthMaterial
											? shadowMesh.m_customDepthMaterial->GetVersionForSubmission(
												  materialSubmissionId)
											: RHIMaterialVersionPtr{},
										BuildPackedDrawStableKey(proxy.m_handle, proxy.GetProducerKey(), 0u,
											static_cast<uint32_t>(shadowMeshIndex), 0u));
								}

								const auto& topology = proxy.m_resource->m_proxy;
								for (size_t groupIndex = 0u; groupIndex < topology.m_instancedGroups.Num();
									++groupIndex)
								{
									const auto& group = topology.m_instancedGroups[groupIndex];
									if (!group.m_bCastShadows)
									{
										continue;
									}
									for (size_t meshIndex = 0u; meshIndex < group.m_meshes.Num(); ++meshIndex)
									{
										const size_t renderQueueTag = meshIndex < group.m_renderQueueTags.Num()
											? group.m_renderQueueTags[meshIndex]
											: 0u;
										ShaderSetPtr customDepthShader;
										RHIMaterialPtr customDepthMaterial;
										if (meshIndex < group.m_sourceMaterialShaders.Num() &&
											group.m_sourceMaterialShaders[meshIndex] &&
											meshIndex < group.m_materials.Num() && group.m_materials[meshIndex] &&
											group.m_materials[meshIndex]
												->GetRenderState()
												.IsRequiredCustomDepthShader())
										{
											customDepthShader = group.m_sourceMaterialShaders[meshIndex];
											customDepthMaterial = group.m_materials[meshIndex];
										}
										for (size_t instanceIndex = 0u;
											instanceIndex < group.m_instanceTransforms.Num(); ++instanceIndex)
										{
											addArenaShadowInstance(group.m_meshes[meshIndex],
												proxy.ResolveInstancedMeshWorldMatrix(group, instanceIndex, meshIndex),
												renderQueueTag,
												meshIndex < group.m_baseColorFactors.Num()
													? group.m_baseColorFactors[meshIndex].a
													: 1.0f,
												meshIndex < group.m_baseColorSamplers.Num()
													? group.m_baseColorSamplers[meshIndex]
													: 0u,
												meshIndex < group.m_alphaCutoffs.Num() ? group.m_alphaCutoffs[meshIndex]
																					   : 0.5f,
												customDepthShader, customDepthMaterial,
												customDepthMaterial
													? customDepthMaterial->GetVersionForSubmission(materialSubmissionId)
													: RHIMaterialVersionPtr{},
												BuildPackedDrawStableKey(proxy.m_handle, proxy.GetProducerKey(),
													static_cast<uint32_t>(groupIndex + 1u),
													static_cast<uint32_t>(meshIndex),
													static_cast<uint32_t>(instanceIndex)));
										}
									}
								}
								if (!m_pagedArenaCache.ReplaceRange(rangeKey, rangeRevision, rangeInstances,
										rangeStableKeys, &rangeMaterialVersionRuns))
								{
									bShadowPayloadComplete[passIndex][arenaPayloadIndex] = false;
								}
							});

						auto arenaPayload =
							m_pagedArenaCache.EndUpdate(bShadowPayloadComplete[passIndex][arenaPayloadIndex]);
						viewResources.m_packet.UseSharedArenaPayload(mobility, std::move(arenaPayload));
						rangeInstances.Clear(false);
						rangeStableKeys.Clear(false);
						rangeMaterialVersionRuns.Clear(false);
						bBuildShadowPayloads[passIndex][arenaPayloadIndex] = false;
					}
				}
			}
			for (const auto& proxy : shadowPass.m_meshList)
			{
				const auto* source = proxy.GetSource();
				if (!source)
				{
					continue;
				}
				const EMobilityType payloadMobility = proxy.GetMobility();
				const size_t payloadIndex = RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(payloadMobility);
				if (!bBuildShadowPayloads[passIndex][payloadIndex] && !usesPagedArena(payloadIndex))
				{
					continue;
				}

				for (size_t shadowMeshIndex = 0u; shadowMeshIndex < source->m_meshes.Num(); ++shadowMeshIndex)
				{
					const auto& shadowMesh = source->m_meshes[shadowMeshIndex];
					const auto mesh = proxy.ResolveMesh(shadowMesh, shadowPass.m_lightMatrix);
					if (!mesh)
					{
						if (shadowMesh.m_renderQueueTag == opaqueQueueTag ||
							shadowMesh.m_renderQueueTag == maskedQueueTag)
						{
							bShadowPayloadComplete[passIndex][payloadIndex] = false;
						}
						continue;
					}
					const glm::mat4 meshWorldMatrix = proxy.ResolveMeshWorldMatrix(shadowMesh);

					if (shadowMesh.m_maxCameraDistance < (std::numeric_limits<float>::max)())
					{
						Math::AABB worldBounds = mesh->m_bounds;
						worldBounds.Apply(meshWorldMatrix);
						const glm::vec3 cameraPosition(sceneView.m_cameraTransform.m_position);
						const glm::vec3 closestPoint = glm::clamp(cameraPosition, worldBounds.m_min, worldBounds.m_max);
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

					const bool bSkinned = proxy.GetSkeletonOffset() != (std::numeric_limits<uint32_t>::max)() &&
						mesh->m_vertexDescription->HasAttribute(RHI::RHIVertexDescription::DefaultBoneIdsBinding) &&
						mesh->m_vertexDescription->HasAttribute(RHI::RHIVertexDescription::DefaultBoneWeightsBinding);
					const bool bMasked = renderQueueTag == maskedQueueTag;
					auto depthMaterial =
						GetOrAddShadowMaterial(mesh->m_vertexDescription, shadowPass.m_shadowType, bSkinned, bMasked);
					if (shadowMesh.m_customDepthMaterial)
					{
						auto customShadowMaterial = GetOrAddCustomShadowMaterial(shadowMesh.m_customDepthShader,
							shadowMesh.m_customDepthMaterial,
							shadowMesh.m_customDepthMaterial->GetVersionForSubmission(materialSubmissionId),
							mesh->m_vertexDescription, shadowPass.m_shadowType, bMasked);
						if (customShadowMaterial)
						{
							depthMaterial = customShadowMaterial;
						}
					}

					const bool bIsDepthMaterialReady =
						depthMaterial && depthMaterial->GetVertexShader() && depthMaterial->GetFragmentShader();

					if (!bIsDepthMaterialReady)
					{
						bShadowPayloadComplete[passIndex][payloadIndex] = false;
						continue;
					}
					RHIBatch batch(depthMaterial, mesh);

					ShadowPrepassNode::PerInstanceData data;
					data.model = meshWorldMatrix;
					data.sphereBounds = mesh->m_bounds.ToSphere().GetVec4();
					const auto materialBindings = batch.GetMaterialBindings();
					data.materialInstance =
						materialBindings ? materialBindings->GetStorageInstanceIndex("material") : 0u;
					data.skeletonOffset = bSkinned ? proxy.GetSkeletonOffset() : (std::numeric_limits<uint32_t>::max)();
					data.bakedVolumeScale = vec4(mesh->m_bakedVolumeScale, 1.0f);
					data.baseColorAlpha = shadowMesh.m_baseColorFactor.a;
					data.baseColorSampler = shadowMesh.m_baseColorSampler;
					data.alphaCutoff = shadowMesh.m_alphaCutoff;

					if (bMasked || depthMaterial->GetRenderState().IsRequiredCustomDepthShader())
					{
						uint32_t supportedMeshesPerBatch = (std::numeric_limits<uint32_t>::max)();
#if defined(__APPLE__)
						batch.m_textureBindings = Framegraph::Details::GetTextureBindingSet(m_textureBindingCache,
							shadowMesh.m_materialTextureSamplers, sceneView.m_frame, supportedMeshesPerBatch);
#else
						batch.m_textureBindings = App::GetSubmodule<TextureImporter>()->GetTextureSamplersBindingSet();
#endif
						batch.m_supportedMeshesPerBatch = supportedMeshesPerBatch;
						if (!batch.m_textureBindings)
						{
							bShadowPayloadComplete[passIndex][payloadIndex] = false;
							continue;
						}
					}
					const uint64_t stableKey = BuildPackedDrawStableKey(
						proxy.m_handle, proxy.GetProducerKey(), 0u, static_cast<uint32_t>(shadowMeshIndex), 0u);
					if (usesPagedArena(payloadIndex))
					{
						if (!viewResources.m_packet.AddArenaView(std::move(batch), mesh,
								BuildPackedDrawRangeKey(proxy.m_handle, proxy.GetProducerKey(), proxy.m_resource),
								stableKey, payloadMobility))
						{
							bShadowPayloadComplete[passIndex][payloadIndex] = false;
						}
						continue;
					}

					viewResources.m_packet.Add(std::move(batch), mesh, data, stableKey, payloadMobility);
				}

				const auto* topology = proxy.m_resource ? &proxy.m_resource->m_proxy : nullptr;
				if (!topology)
				{
					continue;
				}

				for (size_t groupIndex = 0u; groupIndex < topology->m_instancedGroups.Num(); ++groupIndex)
				{
					const auto& group = topology->m_instancedGroups[groupIndex];
					if (!group.m_bCastShadows)
					{
						continue;
					}

					for (size_t meshIndex = 0u; meshIndex < group.m_meshes.Num(); ++meshIndex)
					{
						const size_t renderQueueTag =
							meshIndex < group.m_renderQueueTags.Num() ? group.m_renderQueueTags[meshIndex] : 0u;
						if (renderQueueTag != opaqueQueueTag && renderQueueTag != maskedQueueTag)
						{
							continue;
						}

						const auto& sourceMesh = group.m_meshes[meshIndex];
						if (!sourceMesh)
						{
							bShadowPayloadComplete[passIndex][payloadIndex] = false;
							continue;
						}

						const bool bSkinned = proxy.GetSkeletonOffset() != (std::numeric_limits<uint32_t>::max)() &&
							sourceMesh->m_vertexDescription->HasAttribute(
								RHI::RHIVertexDescription::DefaultBoneIdsBinding) &&
							sourceMesh->m_vertexDescription->HasAttribute(
								RHI::RHIVertexDescription::DefaultBoneWeightsBinding);
						const bool bMasked = renderQueueTag == maskedQueueTag;
						auto depthMaterial = GetOrAddShadowMaterial(
							sourceMesh->m_vertexDescription, shadowPass.m_shadowType, bSkinned, bMasked);

						ShaderSetPtr customDepthShader;
						RHIMaterialPtr customDepthMaterial;
						if (meshIndex < group.m_sourceMaterialShaders.Num() &&
							group.m_sourceMaterialShaders[meshIndex] && meshIndex < group.m_materials.Num() &&
							group.m_materials[meshIndex] &&
							group.m_materials[meshIndex]->GetRenderState().IsRequiredCustomDepthShader())
						{
							customDepthShader = group.m_sourceMaterialShaders[meshIndex];
							customDepthMaterial = group.m_materials[meshIndex];
							auto customShadowMaterial = GetOrAddCustomShadowMaterial(customDepthShader,
								customDepthMaterial,
								customDepthMaterial ? customDepthMaterial->GetVersionForSubmission(materialSubmissionId)
													: RHIMaterialVersionPtr{},
								sourceMesh->m_vertexDescription, shadowPass.m_shadowType, bMasked);
							if (customShadowMaterial)
							{
								depthMaterial = customShadowMaterial;
							}
						}

						const bool bIsDepthMaterialReady =
							depthMaterial && depthMaterial->GetVertexShader() && depthMaterial->GetFragmentShader();
						if (!bIsDepthMaterialReady)
						{
							bShadowPayloadComplete[passIndex][payloadIndex] = false;
							continue;
						}

						RHIBatch batchTemplate(depthMaterial, sourceMesh);
						const auto materialBindings = batchTemplate.GetMaterialBindings();
						const uint32_t materialInstance =
							materialBindings ? materialBindings->GetStorageInstanceIndex("material") : 0u;
						if (bMasked || depthMaterial->GetRenderState().IsRequiredCustomDepthShader())
						{
							uint32_t supportedMeshesPerBatch = (std::numeric_limits<uint32_t>::max)();
#if defined(__APPLE__)
							const auto& requestedTextures = meshIndex < group.m_materialTextureSamplers.Num()
								? group.m_materialTextureSamplers[meshIndex]
								: Framegraph::Details::GetDefaultRequestedTextures();
							batchTemplate.m_textureBindings = Framegraph::Details::GetTextureBindingSet(
								m_textureBindingCache, requestedTextures, sceneView.m_frame, supportedMeshesPerBatch);
#else
							batchTemplate.m_textureBindings =
								App::GetSubmodule<TextureImporter>()->GetTextureSamplersBindingSet();
#endif
							batchTemplate.m_supportedMeshesPerBatch = supportedMeshesPerBatch;
							if (!batchTemplate.m_textureBindings)
							{
								bShadowPayloadComplete[passIndex][payloadIndex] = false;
								continue;
							}
						}

						const float baseColorAlpha = meshIndex < group.m_baseColorFactors.Num()
							? group.m_baseColorFactors[meshIndex].a
							: 1.0f;
						const uint32_t baseColorSampler =
							meshIndex < group.m_baseColorSamplers.Num() ? group.m_baseColorSamplers[meshIndex] : 0u;
						const float alphaCutoff =
							meshIndex < group.m_alphaCutoffs.Num() ? group.m_alphaCutoffs[meshIndex] : 0.5f;

						for (size_t instanceIndex = 0u; instanceIndex < group.m_instanceTransforms.Num();
							++instanceIndex)
						{
							if (!proxy.IsInstancedMeshWithinDistance(group, instanceIndex, meshIndex,
									glm::vec3(sceneView.m_cameraTransform.m_position), group.m_maxShadowDistance))
							{
								continue;
							}
							const auto mesh =
								proxy.ResolveInstancedMesh(group, instanceIndex, meshIndex, shadowPass.m_lightMatrix);
							if (!mesh)
							{
								bShadowPayloadComplete[passIndex][payloadIndex] = false;
								continue;
							}
							const uint64_t stableKey = BuildPackedDrawStableKey(proxy.m_handle, proxy.GetProducerKey(),
								static_cast<uint32_t>(groupIndex + 1u), static_cast<uint32_t>(meshIndex),
								static_cast<uint32_t>(instanceIndex));
							const glm::mat4 meshWorldMatrix =
								proxy.ResolveInstancedMeshWorldMatrix(group, instanceIndex, meshIndex);
							RHIBatch batch = batchTemplate;
							batch.m_mesh = mesh;
							if (usesPagedArena(payloadIndex))
							{
								if (!viewResources.m_packet.AddArenaView(std::move(batch), mesh,
										BuildPackedDrawRangeKey(
											proxy.m_handle, proxy.GetProducerKey(), proxy.m_resource),
										stableKey, payloadMobility))
								{
									bShadowPayloadComplete[passIndex][payloadIndex] = false;
								}
								continue;
							}

							ShadowPrepassNode::PerInstanceData data;
							data.model = meshWorldMatrix;
							data.sphereBounds = mesh->m_bounds.ToSphere().GetVec4();
							data.materialInstance = materialInstance;
							data.skeletonOffset =
								bSkinned ? proxy.GetSkeletonOffset() : (std::numeric_limits<uint32_t>::max)();
							data.bakedVolumeScale = vec4(mesh->m_bakedVolumeScale, 1.0f);
							data.baseColorAlpha = baseColorAlpha;
							data.baseColorSampler = baseColorSampler;
							data.alphaCutoff = alphaCutoff;

							viewResources.m_packet.Add(std::move(batch), mesh, data, stableKey, payloadMobility);
						}
					}
				}
			}
			auto& packet = submissionResources->m_activeShadowViews[passIndex]->m_packet;
			packet.Finalize(false);
			bool bPassPayloadComplete = true;
			for (bool bPayloadComplete : bShadowPayloadComplete[passIndex])
			{
				bPassPayloadComplete &= bPayloadComplete;
			}
			if (shadowPass.m_payloadCompletionToken)
			{
				shadowPass.m_payloadCompletionToken->Complete(
					bPassPayloadComplete);
			}
			for (const EMobilityType mobility :
				{ EMobilityType::Static, EMobilityType::Stationary })
			{
				const size_t index =
					RHI::TPackedDrawPacket<PerInstanceData>::ToSegmentIndex(mobility);
				if (usesPagedArena(index))
				{
					continue;
				}
				if (bVirtualizeInstancePayloads &&
					bBuildShadowPayloads[passIndex][index] &&
					bShadowPayloadComplete[passIndex][index])
				{
					m_packetPayloadCache.Publish(
						buildPayloadCacheSlot(
							viewResources.m_viewKey,
							shadowPass.m_shadowType,
							mobility),
						shadowPayloadRevisions[passIndex][index],
						packet.SharePayload(mobility),
						sceneView.m_frame);
				}
			}
		}
		m_pagedArenaCache.Evict(sceneView.m_frame);

		for (uint32_t passIndex = 0u; passIndex < NumShadowPasses; ++passIndex)
		{
			auto& viewResources = *submissionResources->m_activeShadowViews[passIndex];
			const uint32_t numInstances = viewResources.m_packet.GetNumStorageInstances();
			const uint32_t numInstanceIndices = viewResources.m_packet.GetNumDrawInstances();
			if (numInstances == 0u || numInstanceIndices == 0u)
			{
				continue;
			}
			if (!viewResources.m_perInstanceData ||
				viewResources.m_sizePerInstanceData < sizeof(PerInstanceData) * numInstances ||
				viewResources.m_sizeInstanceIndices < sizeof(uint32_t) * numInstanceIndices)
			{
				viewResources.m_perInstanceData = driver->CreateShaderBindings();
				driver->AddSsboToShaderBindings(
					viewResources.m_perInstanceData,
					"data",
					sizeof(PerInstanceData),
					numInstances,
					0u);
				driver->AddSsboToShaderBindings(
					viewResources.m_perInstanceData,
					"indices",
					sizeof(uint32_t),
					numInstanceIndices,
					1u);
				viewResources.m_sizePerInstanceData = sizeof(PerInstanceData) * numInstances;
				viewResources.m_sizeInstanceIndices = sizeof(uint32_t) * numInstanceIndices;
			}
		}

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

				renderPassColorAttachments.Clear(false);
				renderPassColorAttachments.Add(shadowPass.m_shadowMap);
				commands->BeginRenderPass(commandList,
					renderPassColorAttachments,
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
				auto& currentViewResources = *submissionResources->m_activeShadowViews[index];
				if (!currentViewResources.m_packet.GetGroups().IsEmpty())
				{
					pushConstantsMaterial = currentViewResources.m_packet.GetGroups()[0].m_batch.m_material;
				}
				else
				{
					for (uint32_t dependencyPass : shadowPass.m_internalCommandsList)
					{
						if (dependencyPass < submissionResources->m_activeShadowViews.Num() &&
							!submissionResources->m_activeShadowViews[dependencyPass]->m_packet.GetGroups().IsEmpty())
						{
							pushConstantsMaterial = submissionResources->m_activeShadowViews[dependencyPass]
								->m_packet.GetGroups()[0].m_batch.m_material;
							break;
						}
					}
				}

				if (pushConstantsMaterial)
				{
					commands->PushConstants(commandList, pushConstantsMaterial, 64, &shadowPass.m_lightMatrix);
				}

				auto recordShadowPacket = [&](uint32_t packetIndex)
				{
					if (packetIndex >= submissionResources->m_activeShadowViews.Num())
					{
						return;
					}
					auto& viewResources = *submissionResources->m_activeShadowViews[packetIndex];
					if (viewResources.m_packet.GetGroups().IsEmpty() || !viewResources.m_perInstanceData)
					{
						return;
					}
					const auto collectShaderBindingsByMaterial = [&](
						const RHIBatch& batch,
						TVector<RHIShaderBindingSetPtr>& sets)
					{
						if (batch.m_material->GetRenderState().IsRequiredCustomDepthShader())
						{
							sets.Add(sceneView.m_frameBindings);
							sets.Add(sceneView.m_rhiLightsData);
							sets.Add(viewResources.m_perInstanceData);
							sets.Add(batch.GetMaterialBindings());
							sets.Add(batch.m_textureBindings);
						}
						else
						{
							sets.Add(sceneView.m_frameBindings);
							sets.Add(viewResources.m_perInstanceData);
							if (batch.m_textureBindings)
							{
								sets.Add(batch.m_textureBindings);
							}
						}
						const bool bSkinned =
							batch.m_mesh->m_vertexDescription->HasAttribute(RHIVertexDescription::DefaultBoneIdsBinding) &&
							batch.m_mesh->m_vertexDescription->HasAttribute(RHIVertexDescription::DefaultBoneWeightsBinding);
						if (bSkinned && sceneView.m_boneMatrices)
						{
							sets.Add(sceneView.m_boneMatrices);
						}
					};

					m_drawCallStats += RHIRecordPackedDrawPacket(
						viewResources.m_packet,
						commandList,
						transferCommandList,
						collectShaderBindingsByMaterial,
						viewResources.m_perInstanceData,
						viewResources.m_indirectBuffer,
						glm::ivec4(renderArea.x, renderArea.y + renderArea.w, renderArea.z, -renderArea.w),
						glm::uvec4(renderArea),
						glm::vec2(0.0f, 1.0f));
					viewResources.m_bUploadedThisSubmission = true;
				};

				if (pushConstantsMaterial)
				{
					recordShadowPacket(index);
				}
				for (uint32_t dependencyPass : shadowPass.m_internalCommandsList)
				{
					if (pushConstantsMaterial)
					{
						recordShadowPacket(dependencyPass);
					}
				}

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
						driver->AddSamplerToShaderBindings(
							blurShaderBindings,
							"colorSampler",
							shadowPass.m_shadowMap,
							1);
						blurShaderBindings->RecalculateCompatibility();

						commands->ImageMemoryBarrier(commandList, shadowPass.m_shadowMap, EImageLayout::ShaderReadOnlyOptimal);
						commands->ImageMemoryBarrier(commandList, blurAttachment, EImageLayout::ColorAttachmentOptimal);

						renderPassColorAttachments.Clear(false);
						renderPassColorAttachments.Add(blurAttachment);
						commands->BeginRenderPass(commandList,
							renderPassColorAttachments,
							nullptr,
							glm::vec4(0, 0, shadowPass.m_shadowMap->GetExtent().x, shadowPass.m_shadowMap->GetExtent().y),
							glm::ivec2(0, 0),
							false,
							glm::vec4(0.0f),
							0.0f,
							false);

						commands->BindMaterial(commandList, m_pBlurHorizontalMaterial);
						blurDrawBindingSets.Clear(false);
						blurDrawBindingSets.Add(sceneView.m_frameBindings);
						blurDrawBindingSets.Add(blurShaderBindings);
						commands->BindShaderBindings(
							commandList,
							m_pBlurHorizontalMaterial,
							blurDrawBindingSets);

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
						driver->AddSamplerToShaderBindings(
							blurShaderBindings,
							"colorSampler",
							blurAttachment,
							1);
						blurShaderBindings->RecalculateCompatibility();

						renderPassColorAttachments.Clear(false);
						renderPassColorAttachments.Add(shadowPass.m_shadowMap);
						commands->BeginRenderPass(commandList,
							renderPassColorAttachments,
							nullptr,
							glm::vec4(0, 0, shadowPass.m_shadowMap->GetExtent().x, shadowPass.m_shadowMap->GetExtent().y),
							glm::ivec2(0, 0),
							false,
							glm::vec4(0.0f),
							0.0f,
							false);

						commands->BindMaterial(commandList, m_pBlurVerticalMaterial);
						blurDrawBindingSets.Clear(false);
						blurDrawBindingSets.Add(sceneView.m_frameBindings);
						blurDrawBindingSets.Add(blurShaderBindings);
						commands->BindShaderBindings(
							commandList,
							m_pBlurVerticalMaterial,
							blurDrawBindingSets);

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
	m_packetPayloadCache.Clear();
	m_pagedArenaCache.Clear();
}

glm::mat4 ShadowPrepassNode::CalculateLightProjectionMatrix(const glm::mat4& lightView, const glm::mat4& cameraWorld, float aspect, float fovY, float zNear, float zFar, float zMult, glm::ivec2 shadowMapResolution, float zSourceExtension)
{
	SAILOR_PROFILE_FUNCTION();

	Math::Frustum cameraFrustum{};
	cameraFrustum.ExtractFrustumPlanes(cameraWorld, aspect, fovY, zNear, zFar);
	return cameraFrustum.CalculateOrthoMatrixByView(lightView, zMult, shadowMapResolution, zSourceExtension);
}

void ShadowPrepassNode::CalculateLightProjectionForCascades(
	const glm::mat4& lightView,
	const glm::mat4& cameraWorld,
	float aspect,
	float fovY,
	float cameraNearPlane,
	float cameraFarPlane,
	TVector<glm::mat4>& outMatrices)
{
	SAILOR_PROFILE_FUNCTION();
	const float shadowFarPlane = (std::min)(cameraFarPlane, LightingECS::ShadowMaxDistance);

	outMatrices.Clear(false);
	outMatrices.Reserve(LightingECS::NumCascades);
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

		outMatrices.Add(CalculateLightProjectionMatrix(lightView, cameraWorld, aspect, fovY,
			cascadeNear,
			cascadeFar,
			10.0f,
			LightingECS::ShadowCascadeResolutions[i],
			LightingECS::ShadowCasterDepthExtension));
	}
}
