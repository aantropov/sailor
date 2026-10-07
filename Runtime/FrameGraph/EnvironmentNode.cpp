#include "EnvironmentNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/RenderTarget.h"
#include "RHI/Texture.h"
#include "RHI/Cubemap.h"
#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "FrameGraph/SkyNode.h"
#include "FrameGraph/EnvironmentSource.h"
#include "AssetRegistry/AssetRegistry.h"
#include "Tasks/Tasks.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

bool EnvironmentNode::TryRestoreEnvironment(RHIFrameGraphPtr frameGraph,
	const SkyEnvironmentKey& key, RHICubemapPtr rawCubemap)
{
	for (uint32_t index = 0u; index < m_numCachedEnvironments; ++index)
	{
		if (m_environmentCache[index].m_key == key)
		{
			CachedEnvironment entry = std::move(m_environmentCache[index]);
			for (uint32_t next = index + 1u; next < m_numCachedEnvironments; ++next)
			{
				m_environmentCache[next - 1u] = std::move(m_environmentCache[next]);
			}
			m_environmentCache[m_numCachedEnvironments - 1u] = std::move(entry);
			const EnvironmentMaps& maps = m_environmentCache[m_numCachedEnvironments - 1u].m_maps;
			frameGraph->SetSampler("g_rawEnvCubemap"_h, rawCubemap);
			frameGraph->SetSampler("g_envCubemap"_h, maps.m_specular);
			frameGraph->SetSampler("g_irradianceCubemap"_h, maps.m_irradiance);
			frameGraph->SetSampler("g_sheenEnvCubemap"_h, maps.m_sheen);
			return true;
		}
	}
	return false;
}

void EnvironmentNode::CacheEnvironment(const SkyEnvironmentKey& key, EnvironmentMaps maps)
{
	if (m_numCachedEnvironments == m_environmentCache.size())
	{
		for (uint32_t index = 1u; index < m_numCachedEnvironments; ++index)
		{
			m_environmentCache[index - 1u] = std::move(m_environmentCache[index]);
		}
	}
	else
	{
		++m_numCachedEnvironments;
	}
	m_environmentCache[m_numCachedEnvironments - 1u] = { key, std::move(maps) };
}

void EnvironmentNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdCompute);

	SetTag("Environment"_h);

	if (!m_pComputeBrdfShader)
	{
		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeBrdfLut.shader"))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderInfo->GetFileId(), m_pComputeBrdfShader);
		}

		m_computeBrdfBindings = driver->CreateShaderBindings();
	}

	if (!m_pComputeSpecularShader)
	{
		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeEnvMap_IBL.shader"))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderInfo->GetFileId(), m_pComputeSpecularShader);
		}

		m_computeSpecularBindings = driver->CreateShaderBindings();
	}

	if (!m_pComputeSheenShader)
	{
		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeSheenEnvMap.shader"))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderInfo->GetFileId(), m_pComputeSheenShader);
		}

		m_computeSheenBindings = driver->CreateShaderBindings();
	}

	if (!m_pComputeIrradianceShader)
	{
		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeIrradianceMap.shader"))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderInfo->GetFileId(), m_pComputeIrradianceShader);
		}

		m_computeIrradianceBindings = driver->CreateShaderBindings();
	}

	const RHI::ETextureUsageFlags usage = RHI::ETextureUsageBit::ColorAttachment_Bit |
		RHI::ETextureUsageBit::TextureTransferSrc_Bit |
		RHI::ETextureUsageBit::TextureTransferDst_Bit |
		RHI::ETextureUsageBit::Storage_Bit |
		RHI::ETextureUsageBit::Sampled_Bit;

	if (!m_brdfSampler)
	{
		m_brdfSampler = RHI::Renderer::GetDriver()->CreateRenderTarget(ivec2(BrdfLutSize, BrdfLutSize), 1,
			RHI::EFormat::R16G16B16A16_SFLOAT, RHI::ETextureFiltration::Linear,
			RHI::ETextureClamping::Clamp, usage);
		if (!m_brdfSampler)
		{
			commands->EndDebugRegion(commandList);
			return;
		}

		commands->ImageMemoryBarrier(commandList, m_brdfSampler, EImageLayout::ShaderReadOnlyOptimal);
		m_brdfSampler->ForceSetDefaultLayout(EImageLayout::ShaderReadOnlyOptimal);

		RHI::Renderer::GetDriver()->SetDebugName(m_brdfSampler, "g_brdfSampler"_h);
		frameGraph->SetSampler("g_brdfSampler"_h, m_brdfSampler);

		commands->BeginDebugRegion(commandList, "Generate Cook-Torrance BRDF 2D LUT for split-sum approximation"_h, DebugContext::Color_CmdCompute);
		{
			driver->AddStorageImageToShaderBindings(m_computeBrdfBindings, "dst"_h, m_brdfSampler, 0);
			commands->ImageMemoryBarrier(commandList, m_brdfSampler, EImageLayout::ComputeWrite);

			commands->Dispatch(commandList, m_pComputeBrdfShader->GetComputeShaderRHI(),
				(uint32_t)(m_brdfSampler->GetExtent().x / 32.0f),
				(uint32_t)(m_brdfSampler->GetExtent().y / 32.0f),
				1u,
				{ m_computeBrdfBindings },
				nullptr, 0);

			commands->ImageMemoryBarrier(commandList, m_brdfSampler, EImageLayout::ShaderReadOnlyOptimal);
		}
		commands->EndDebugRegion(commandList);
	}

	ProcessLocalReflection(frameGraph, commandList);

	if (m_bIsDirty)
	{
		std::string environmentMap;
		TryGetString("EnvironmentMap"_h, environmentMap);
		TRefPtr<SkyNode> skyNode;
		SkyParameters skyParameters;
		if (environmentMap.empty())
		{
			if (auto node = frameGraph->GetGraphNode("Sky"_h)) skyNode = node.DynamicCast<SkyNode>();
			if (skyNode && !skyNode->GetEnvironmentSkyParams(skyParameters))
			{
				commands->EndDebugRegion(commandList);
				return;
			}
		}
		EnvironmentSource source;
		std::string diagnostic;
		if (!CaptureEnvironmentSource(environmentMap, skyNode ? &skyParameters : nullptr, source, diagnostic))
		{
			commands->EndDebugRegion(commandList);
			return;
		}
		if (source.m_type == EEnvironmentSource::Texture)
		{
			if (!m_envMapTexture || m_envMapTexture->GetFileId() != source.m_texture.m_fileId)
			{
				TexturePtr texture;
				if (App::GetSubmodule<TextureImporter>()->LoadTexture_Immediate(source.m_texture.m_fileId, texture))
					m_envMapTexture = std::move(texture);
				commands->EndDebugRegion(commandList);
				return;
			}
		}
		else if (m_envMapTexture)
		{
			m_envMapTexture.Clear();
			m_authoredSource.Clear();
			m_staticRawCubemap.Clear();
			m_numCachedEnvironments = 0u;
		}

		RHI::RHICubemapPtr rawEnvCubemap{};
		RHI::RHITexturePtr authoredSource;

		if (m_envMapTexture)
		{
			if (!m_envMapTexture->IsReady())
			{
				commands->EndDebugRegion(commandList);
				return;
			}

			// Reload preserves the Texture object but publishes a new RHI image.
			authoredSource = m_envMapTexture->GetRHI();
			if (authoredSource == m_authoredSource && TryRestoreEnvironment(frameGraph, {}, m_staticRawCubemap))
			{
				m_environmentUsesSky = false;
				m_bIsDirty = false;
				commands->EndDebugRegion(commandList);
				return;
			}

			rawEnvCubemap = RHI::Renderer::GetDriver()->CreateCubemap(ivec2(EnvMapSize, EnvMapSize),
				EnvMapLevels,
				RHI::EFormat::R16G16B16A16_SFLOAT,
				RHI::ETextureFiltration::Linear,
				RHI::ETextureClamping::Clamp,
				usage);

			if (!rawEnvCubemap)
			{
				commands->EndDebugRegion(commandList);
				return;
			}
			commands->ImageMemoryBarrier(commandList, rawEnvCubemap, EImageLayout::ShaderReadOnlyOptimal);
			rawEnvCubemap->ForceSetDefaultLayout(EImageLayout::ShaderReadOnlyOptimal);

			RHI::Renderer::GetDriver()->SetDebugName(rawEnvCubemap, "rawEnvCubemap"_h);

			commands->BeginDebugRegion(commandList, "Generate Raw Env Cubemap from Equirect"_h, DebugContext::Color_CmdCompute);
			{
				commands->ImageMemoryBarrier(commandList, rawEnvCubemap, EImageLayout::ComputeWrite);
				commands->ConvertEquirect2Cubemap(commandList, authoredSource, rawEnvCubemap);
				commands->ImageMemoryBarrier(commandList, rawEnvCubemap, EImageLayout::TransferDstOptimal);

				commands->GenerateMipMaps(commandList, rawEnvCubemap);
			}
			commands->EndDebugRegion(commandList);
		}
		else if (source.m_type == EEnvironmentSource::Constant)
		{
			if (m_staticRawCubemap && TryRestoreEnvironment(frameGraph, {}, m_staticRawCubemap))
			{
				m_environmentUsesSky = false;
				m_bIsDirty = false;
				commands->EndDebugRegion(commandList);
				return;
			}
			rawEnvCubemap = driver->CreateCubemap(ivec2(EnvMapSize), EnvMapLevels,
				EFormat::R16G16B16A16_SFLOAT, ETextureFiltration::Linear, ETextureClamping::Clamp, usage);
			if (!rawEnvCubemap)
			{
				commands->EndDebugRegion(commandList);
				return;
			}
			commands->ImageMemoryBarrier(commandList, rawEnvCubemap, EImageLayout::TransferDstOptimal);
			commands->ClearImage(commandList, rawEnvCubemap, glm::vec4(source.m_constant, 1.0f));
			commands->ImageMemoryBarrier(commandList, rawEnvCubemap, EImageLayout::ShaderReadOnlyOptimal);
			rawEnvCubemap->ForceSetDefaultLayout(EImageLayout::ShaderReadOnlyOptimal);
		}
		else if (auto g_skyCubemap = frameGraph->GetSampler("g_skyCubemap"_h).DynamicCast<RHICubemap>())
		{
			rawEnvCubemap = g_skyCubemap;
		}
		else
		{
			commands->EndDebugRegion(commandList);
			return;
		}

		const bool bUsesSky = source.m_type == EEnvironmentSource::Sky;
		const auto skyHash = bUsesSky ? source.m_sky.GetEnvironmentKey() : SkyEnvironmentKey{};
		if (bUsesSky && TryRestoreEnvironment(frameGraph, skyHash, rawEnvCubemap))
		{
			m_environmentUsesSky = true;
			m_environmentSkyParams = source.m_sky;
			m_bIsDirty = false;
			commands->EndDebugRegion(commandList);
			return;
		}

		EnvironmentMaps maps;
		auto& envCubemap = maps.m_specular;
		auto& irradianceCubemap = maps.m_irradiance;
		auto& sheenEnvCubemap = maps.m_sheen;

		{
			envCubemap = RHI::Renderer::GetDriver()->CreateCubemap(ivec2(EnvMapSize, EnvMapSize),
				EnvMapLevels,
				RHI::EFormat::R16G16B16A16_SFLOAT,
				RHI::ETextureFiltration::Linear,
				RHI::ETextureClamping::Clamp,
				usage);

			if (!envCubemap)
			{
				commands->EndDebugRegion(commandList);
				return;
			}
			RHI::Renderer::GetDriver()->SetDebugName(envCubemap, "g_envCubemap"_h);

			commands->ImageMemoryBarrier(commandList, envCubemap, EImageLayout::General);

			commands->BeginDebugRegion(commandList, "Compute pre-filtered specular environment map"_h, DebugContext::Color_CmdCompute);
			{
				struct PushConstants { int32_t level{}; float roughness{}; };
				const uint32_t NumMipTailLevels = EnvMapLevels - 1;

				commands->ImageMemoryBarrier(commandList, rawEnvCubemap, EImageLayout::TransferSrcOptimal);
				commands->ImageMemoryBarrier(commandList, envCubemap, EImageLayout::TransferDstOptimal);

				commands->BlitImage(commandList, rawEnvCubemap, envCubemap,
					glm::ivec4(0, 0, rawEnvCubemap->GetExtent().x, rawEnvCubemap->GetExtent().y),
					glm::ivec4(0, 0, envCubemap->GetExtent().x, envCubemap->GetExtent().y));

				commands->ImageMemoryBarrier(commandList, rawEnvCubemap, EImageLayout::ShaderReadOnlyOptimal);
				commands->ImageMemoryBarrier(commandList, envCubemap, EImageLayout::ComputeWrite);

				// Pre-filter rest of the mip-chain.
				TVector<RHI::RHITexturePtr> envMapMips;
				for (uint32_t level = 1; level < EnvMapLevels; ++level)
				{
					envMapMips.Add(envCubemap->GetMipLevel(level));
				}

				driver->AddSamplerToShaderBindings(m_computeSpecularBindings, "rawEnvMap"_h, rawEnvCubemap, 0);
				driver->AddStorageImageToShaderBindings(m_computeSpecularBindings, "envMap"_h, envMapMips, 1);

				m_computeSpecularBindings->RecalculateCompatibility();

				const float deltaRoughness = 1.0f / std::max(float(NumMipTailLevels), 1.0f);
				for (uint32_t level = 1, size = EnvMapSize / 2; level < EnvMapLevels; ++level, size /= 2)
				{
					const uint32_t numGroups = std::max<uint32_t>(1u, size / 32u);
					const PushConstants pushConstants = { (int32_t)(level - 1u), level * deltaRoughness };

					commands->Dispatch(commandList,
						m_pComputeSpecularShader->GetComputeShaderRHI(),
						numGroups,
						numGroups,
						6u,
						{ m_computeSpecularBindings },
						&pushConstants, sizeof(PushConstants));
				}
			}
			commands->ImageMemoryBarrier(
				commandList,
				envCubemap,
				EImageLayout::ShaderReadOnlyOptimal);
			commands->EndDebugRegion(commandList);
		}

		{
			irradianceCubemap = RHI::Renderer::GetDriver()->CreateCubemap(ivec2(IrradianceMapSize, IrradianceMapSize),
				1,
				RHI::EFormat::R16G16B16A16_SFLOAT,
				RHI::ETextureFiltration::Linear,
				RHI::ETextureClamping::Clamp,
				usage);
			
			if (!irradianceCubemap)
			{
				commands->EndDebugRegion(commandList);
				return;
			}
			commands->ImageMemoryBarrier(commandList, irradianceCubemap, EImageLayout::ShaderReadOnlyOptimal);
			irradianceCubemap->ForceSetDefaultLayout(EImageLayout::ShaderReadOnlyOptimal);

			RHI::Renderer::GetDriver()->SetDebugName(irradianceCubemap, "g_irradianceCubemap"_h);

			commands->ImageMemoryBarrier(commandList, irradianceCubemap, EImageLayout::General);

			commands->BeginDebugRegion(commandList, "Compute diffuse irradiance cubemap"_h, DebugContext::Color_CmdCompute);
			{
				commands->ImageMemoryBarrier(commandList, envCubemap, EImageLayout::ShaderReadOnlyOptimal);
				commands->ImageMemoryBarrier(commandList, irradianceCubemap, EImageLayout::ComputeWrite);

				driver->AddSamplerToShaderBindings(m_computeIrradianceBindings, "envMap"_h, envCubemap, 0);
				driver->AddStorageImageToShaderBindings(m_computeIrradianceBindings, "irradianceMap"_h, irradianceCubemap, 1);

				m_computeIrradianceBindings->RecalculateCompatibility();

				commands->Dispatch(commandList,
					m_pComputeIrradianceShader->GetComputeShaderRHI(),
					IrradianceMapSize / 32u,
					IrradianceMapSize / 32u,
					6u,
					{ m_computeIrradianceBindings });
			}
			commands->ImageMemoryBarrier(
				commandList,
				irradianceCubemap,
				EImageLayout::ShaderReadOnlyOptimal);
			commands->EndDebugRegion(commandList);
		}

		{
			sheenEnvCubemap = RHI::Renderer::GetDriver()->CreateCubemap(
				ivec2(SheenEnvMapSize, SheenEnvMapSize),
				SheenEnvMapLevels,
				RHI::EFormat::R16G16B16A16_SFLOAT,
				RHI::ETextureFiltration::Linear,
				RHI::ETextureClamping::Clamp,
				usage);

			if (!sheenEnvCubemap)
			{
				commands->EndDebugRegion(commandList);
				return;
			}
			commands->ImageMemoryBarrier(
				commandList,
				sheenEnvCubemap,
				EImageLayout::ShaderReadOnlyOptimal);
			sheenEnvCubemap->ForceSetDefaultLayout(
				EImageLayout::ShaderReadOnlyOptimal);
			RHI::Renderer::GetDriver()->SetDebugName(
				sheenEnvCubemap,
				"g_sheenEnvCubemap"_h);

			commands->BeginDebugRegion(
				commandList,
				"Compute Charlie pre-filtered sheen environment map"_h,
				DebugContext::Color_CmdCompute);
			{
				struct PushConstants
				{
					int32_t level{};
					float roughness{};
				};
				TVector<RHI::RHITexturePtr> sheenEnvMapMips;
				for (uint32_t level = 0u;
					level < SheenEnvMapLevels;
					++level)
				{
					sheenEnvMapMips.Add(
						level == 0u
							? sheenEnvCubemap
							: sheenEnvCubemap->GetMipLevel(level));
				}

				driver->AddSamplerToShaderBindings(
					m_computeSheenBindings,
					"rawEnvMap"_h,
					rawEnvCubemap,
					0u);
				driver->AddStorageImageToShaderBindings(
					m_computeSheenBindings,
					"sheenEnvMap"_h,
					sheenEnvMapMips,
					1u);
				m_computeSheenBindings->RecalculateCompatibility();

				commands->ImageMemoryBarrier(
					commandList,
					rawEnvCubemap,
					EImageLayout::ShaderReadOnlyOptimal);
				commands->ImageMemoryBarrier(
					commandList,
					sheenEnvCubemap,
					EImageLayout::ComputeWrite);

				const float deltaRoughness = 1.0f /
					std::max(float(SheenEnvMapLevels - 1u), 1.0f);
				for (uint32_t level = 0u, size = SheenEnvMapSize;
					level < SheenEnvMapLevels;
					++level, size = std::max(size / 2u, 1u))
				{
					const uint32_t numGroups =
						std::max<uint32_t>(1u, (size + 15u) / 16u);
					const PushConstants pushConstants = {
						static_cast<int32_t>(level),
						level * deltaRoughness
					};
					commands->Dispatch(
						commandList,
						m_pComputeSheenShader->GetComputeShaderRHI(),
						numGroups,
						numGroups,
						6u,
						{ m_computeSheenBindings },
						&pushConstants,
						sizeof(PushConstants));
				}
			}
			commands->ImageMemoryBarrier(
				commandList,
				sheenEnvCubemap,
				EImageLayout::ShaderReadOnlyOptimal);
			commands->EndDebugRegion(commandList);
		}

		frameGraph->SetSampler("g_rawEnvCubemap"_h, rawEnvCubemap);
		frameGraph->SetSampler("g_envCubemap"_h, envCubemap);
		frameGraph->SetSampler("g_irradianceCubemap"_h, irradianceCubemap);
		frameGraph->SetSampler("g_sheenEnvCubemap"_h, sheenEnvCubemap);
		m_environmentUsesSky = bUsesSky;
		if (bUsesSky) m_environmentSkyParams = source.m_sky;
		if (source.m_type != EEnvironmentSource::Sky)
		{
			// Authored revisions replace the previous bundle; sky states retain their LRU.
			m_environmentCache = {};
			m_numCachedEnvironments = 0;
			m_authoredSource = std::move(authoredSource);
			m_staticRawCubemap = rawEnvCubemap;
		}
		CacheEnvironment(skyHash, std::move(maps));
		m_bIsDirty = false;
	}

	commands->EndDebugRegion(commandList);
}

void EnvironmentNode::Clear()
{
	m_localReflection.Clear();
	m_localUploadTexture.Clear();
	m_localParameters = {};
	m_bLocalReflectionDirty = true;
	m_localReflectionReady.store(false);
	m_localReflectionSamples.store(0u);
}

bool EnvironmentNode::SetLocalReflection(LocalReflectionImage image)
{
	if (!image.IsValid())
	{
		return false;
	}
	image.m_parameters.m_minEnabled.w = 1.0f;
	Tasks::CreateTask("Update local reflection"_h,
		[node = TRefPtr<EnvironmentNode>(this), image = TSharedPtr<const LocalReflectionImage>::Make(std::move(image))]() mutable
		{
			node->m_localReflection = std::move(image);
			node->m_localUploadTexture.Clear();
			node->m_bLocalReflectionDirty = true;
		}, EThreadType::Render)->Run();
	return true;
}

void EnvironmentNode::ResetLocalReflection()
{
	Tasks::CreateTask("Reset local reflection"_h, [node = TRefPtr<EnvironmentNode>(this)]() mutable
		{
			node->Clear();
		}, EThreadType::Render)->Run();
}

void EnvironmentNode::ProcessLocalReflection(RHIFrameGraphPtr frameGraph, RHICommandListPtr commandList)
{
	if (!m_bLocalReflectionDirty)
	{
		return;
	}
	if (!m_localReflection)
	{
		frameGraph->SetSampler("g_localEnvCubemap"_h, {});
		frameGraph->SetSampler("g_localSheenEnvCubemap"_h, {});
		m_bLocalReflectionDirty = false;
		return;
	}
	if (!m_pComputeSpecularShader || !m_pComputeSheenShader) return;
	auto& driver = RHI::Renderer::GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	if (!m_localUploadTexture)
	{
		const auto& source = *m_localReflection;
		m_localUploadTexture = driver->CreateTexture(source.m_pixels.GetData(),
			source.m_pixels.Num() * sizeof(glm::vec4), glm::ivec3(source.m_extent, 1), 1u,
			ETextureType::Texture2D, ETextureFormat::R32G32B32A32_SFLOAT,
			ETextureFiltration::Linear, ETextureClamping::Repeat);
		return;
	}
	if (!m_localUploadTexture->IsReady()) return;

	constexpr uint32_t size = 128u, levels = 8u;
	const ETextureUsageFlags usage = ETextureUsageBit::TextureTransferSrc_Bit |
		ETextureUsageBit::TextureTransferDst_Bit | ETextureUsageBit::Storage_Bit | ETextureUsageBit::Sampled_Bit;
	const auto createCube = [&]()
	{
		return driver->CreateCubemap(glm::ivec2(size), levels, EFormat::R16G16B16A16_SFLOAT,
			ETextureFiltration::Linear, ETextureClamping::Clamp, usage);
	};
	auto raw = createCube();
	if (!raw) return;
	auto specular = createCube();
	if (!specular) return;
	auto sheen = createCube();
	if (!sheen) return;
	commands->BeginDebugRegion(commandList, "Local scene reflection"_h, DebugContext::Color_CmdCompute);
	commands->ImageMemoryBarrier(commandList, raw, EImageLayout::ComputeWrite);
	commands->ConvertEquirect2Cubemap(commandList, m_localUploadTexture, raw);
	commands->ImageMemoryBarrier(commandList, raw, EImageLayout::TransferDstOptimal);
	commands->GenerateMipMaps(commandList, raw);

	const auto prefilter = [&](RHICubemapPtr filtered, bool sheen)
	{
		auto bindings = driver->CreateShaderBindings();
		if (!sheen)
		{
			commands->ImageMemoryBarrier(commandList, raw, EImageLayout::TransferSrcOptimal);
			commands->ImageMemoryBarrier(commandList, filtered, EImageLayout::TransferDstOptimal);
			commands->BlitImage(commandList, raw, filtered, glm::ivec4(0, 0, size, size), glm::ivec4(0, 0, size, size));
		}
		commands->ImageMemoryBarrier(commandList, raw, EImageLayout::ShaderReadOnlyOptimal);
		commands->ImageMemoryBarrier(commandList, filtered, EImageLayout::ComputeWrite);
		TVector<RHITexturePtr> mips;
		const uint32_t first = sheen ? 0u : 1u;
		for (uint32_t level = first; level < levels; ++level)
			mips.Add(level == 0u ? filtered : filtered->GetMipLevel(level));
		// The existing GGX shader declares nine outputs; unused tail bindings
		// remain valid even though this bounded capture has fewer mip levels.
		while (mips.Num() < (sheen ? 8u : 9u)) mips.Add(filtered->GetMipLevel(levels - 1u));
		driver->AddSamplerToShaderBindings(bindings, "rawEnvMap"_h, raw, 0u);
		driver->AddStorageImageToShaderBindings(bindings, sheen ? "sheenEnvMap"_h : "envMap"_h, mips, 1u);
		bindings->RecalculateCompatibility();
		for (uint32_t level = first; level < levels; ++level)
		{
			struct PushConstants { int32_t level; float roughness; };
			const PushConstants push{ int32_t(level - first), float(level) / float(levels - 1u) };
			const uint32_t groupSize = sheen ? 16u : 32u;
			const uint32_t groups = std::max(1u, ((size >> level) + groupSize - 1u) / groupSize);
			commands->Dispatch(commandList,
				(sheen ? m_pComputeSheenShader : m_pComputeSpecularShader)->GetComputeShaderRHI(),
				groups, groups, 6u, { bindings }, &push, sizeof(push));
		}
		commands->ImageMemoryBarrier(commandList, filtered, EImageLayout::ShaderReadOnlyOptimal);
	};
	prefilter(specular, false);
	prefilter(sheen, true);
	frameGraph->SetSampler("g_localEnvCubemap"_h, specular);
	frameGraph->SetSampler("g_localSheenEnvCubemap"_h, sheen);
	m_localParameters = m_localReflection->m_parameters;
	m_localReflectionSamples.store(m_localReflection->m_samplesPerPixel);
	m_localReflectionReady.store(true);
	m_localReflection.Clear();
	m_localUploadTexture.Clear();
	m_bLocalReflectionDirty = false;
	commands->EndDebugRegion(commandList);
}
