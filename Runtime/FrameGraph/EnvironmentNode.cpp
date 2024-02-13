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


using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* EnvironmentNode::m_name = "Environment";
#endif

void EnvironmentNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdCompute);

	SetTag("Environment");

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
			RHI::EFormat::R16G16_SFLOAT, RHI::ETextureFiltration::Linear,
			RHI::ETextureClamping::Clamp, usage);

		RHI::Renderer::GetDriver()->SetDebugName(m_brdfSampler, "g_brdfSampler");
		frameGraph->SetSampler("g_brdfSampler", m_brdfSampler);

		commands->BeginDebugRegion(commandList, "Generate Cook-Torrance BRDF 2D LUT for split-sum approximation", DebugContext::Color_CmdCompute);
		{
			driver->AddStorageImageToShaderBindings(m_computeBrdfBindings, "dst", m_brdfSampler, 0);
			commands->ImageMemoryBarrier(commandList, m_brdfSampler, m_brdfSampler->GetFormat(), m_brdfSampler->GetDefaultLayout(), EImageLayout::ComputeWrite);

			commands->Dispatch(commandList, m_pComputeBrdfShader->GetComputeShaderRHI(),
				(uint32_t)(m_brdfSampler->GetExtent().x / 32.0f),
				(uint32_t)(m_brdfSampler->GetExtent().y / 32.0f),
				6u,
				{ m_computeBrdfBindings },
				nullptr, 0);

			commands->ImageMemoryBarrier(commandList, m_brdfSampler, m_brdfSampler->GetFormat(), EImageLayout::ComputeWrite, EImageLayout::ShaderReadOnlyOptimal);
		}
		commands->EndDebugRegion(commandList);
	}

	if (m_bIsDirty)
	{
		if (!m_envMapTexture)
		{
			string envMapFilepath;
			if (TryGetString("EnvironmentMap", envMapFilepath))
			{
				if (const auto& assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(envMapFilepath))
				{
					App::GetSubmodule<TextureImporter>()->LoadTexture_Immediate(assetInfo->GetFileId(), m_envMapTexture);
				}
				return;
			}
		}

		RHI::RHICubemapPtr rawEnvCubemap{};
		bool bLoadedEnvironmentMap = false;

		if (m_envMapTexture)
		{
			bLoadedEnvironmentMap = true;
			rawEnvCubemap = RHI::Renderer::GetDriver()->CreateCubemap(ivec2(EnvMapSize, EnvMapSize),
				EnvMapLevels,
				RHI::EFormat::R16G16B16A16_SFLOAT,
				RHI::ETextureFiltration::Linear,
				RHI::ETextureClamping::Clamp,
				usage);
			RHI::Renderer::GetDriver()->SetDebugName(rawEnvCubemap, "rawEnvCubemap");

			commands->BeginDebugRegion(commandList, "Generate Raw Env Cubemap from Equirect", DebugContext::Color_CmdCompute);
			{
				commands->ImageMemoryBarrier(commandList, rawEnvCubemap, rawEnvCubemap->GetFormat(), rawEnvCubemap->GetDefaultLayout(), EImageLayout::ComputeWrite);
				commands->ConvertEquirect2Cubemap(commandList, m_envMapTexture->GetRHI(), rawEnvCubemap);
				commands->ImageMemoryBarrier(commandList, rawEnvCubemap, rawEnvCubemap->GetFormat(), EImageLayout::ComputeWrite, EImageLayout::TransferDstOptimal);

				commands->GenerateMipMaps(commandList, rawEnvCubemap);
			}
			commands->EndDebugRegion(commandList);
		}
		else if (auto g_skyCubemap = frameGraph->GetSampler("g_skyCubemap").DynamicCast<RHICubemap>())
		{
			rawEnvCubemap = g_skyCubemap;
		}
		else
		{
			return;
		}

		SkyNode::SkyParams skyHash{};
		TRefPtr<SkyNode> pSkyNode{};
		if (auto node = frameGraph->GetGraphNode("Sky"))
		{
			pSkyNode = node.DynamicCast<SkyNode>();
			if (!bLoadedEnvironmentMap)
			{
				skyHash = pSkyNode->GetSkyParams();
			}
		}

		auto& envCubemap = m_envCubemaps[skyHash];
		auto& irradianceCubemap = m_irradianceCubemaps[skyHash];

		const bool bShouldUpdateEnvCubemap = !envCubemap;
		const bool bShouldUpdateIrradianceCubemap = !irradianceCubemap;

		if (!bShouldUpdateEnvCubemap && !bShouldUpdateIrradianceCubemap)
		{
			frameGraph->SetSampler("g_envCubemap", envCubemap);
			frameGraph->SetSampler("g_irradianceCubemap", irradianceCubemap);

			m_bIsDirty = false;

			return;
		}

		// Create all textures/cubemaps
		if (bShouldUpdateEnvCubemap)
		{
			envCubemap = RHI::Renderer::GetDriver()->CreateCubemap(ivec2(EnvMapSize, EnvMapSize),
				EnvMapLevels,
				RHI::EFormat::R16G16B16A16_SFLOAT,
				RHI::ETextureFiltration::Linear,
				RHI::ETextureClamping::Clamp,
				usage);

			frameGraph->SetSampler("g_envCubemap", envCubemap);
			RHI::Renderer::GetDriver()->SetDebugName(envCubemap, "g_envCubemap");

			commands->ImageMemoryBarrier(commandList, envCubemap, envCubemap->GetFormat(), envCubemap->GetDefaultLayout(), EImageLayout::General);

			commands->BeginDebugRegion(commandList, "Compute pre-filtered specular environment map", DebugContext::Color_CmdCompute);
			{
				struct PushConstants { int32_t level{}; float roughness{}; };
				const uint32_t NumMipTailLevels = EnvMapLevels - 1;

				commands->ImageMemoryBarrier(commandList, rawEnvCubemap, rawEnvCubemap->GetFormat(), rawEnvCubemap->GetDefaultLayout(), EImageLayout::TransferSrcOptimal);
				commands->ImageMemoryBarrier(commandList, envCubemap, envCubemap->GetFormat(), envCubemap->GetDefaultLayout(), EImageLayout::TransferDstOptimal);

				commands->BlitImage(commandList, rawEnvCubemap, envCubemap,
					glm::ivec4(0, 0, rawEnvCubemap->GetExtent().x, rawEnvCubemap->GetExtent().y),
					glm::ivec4(0, 0, envCubemap->GetExtent().x, envCubemap->GetExtent().y));

				commands->ImageMemoryBarrier(commandList, rawEnvCubemap, rawEnvCubemap->GetFormat(), EImageLayout::TransferSrcOptimal, EImageLayout::ShaderReadOnlyOptimal);
				commands->ImageMemoryBarrier(commandList, envCubemap, envCubemap->GetFormat(), EImageLayout::TransferDstOptimal, EImageLayout::ComputeWrite);

				// Pre-filter rest of the mip-chain.
				TVector<RHI::RHITexturePtr> envMapMips;
				for (uint32_t level = 1; level < EnvMapLevels; ++level)
				{
					envMapMips.Add(envCubemap->GetMipLevel(level));
				}

				driver->AddSamplerToShaderBindings(m_computeSpecularBindings, "rawEnvMap", rawEnvCubemap, 0);
				driver->AddStorageImageToShaderBindings(m_computeSpecularBindings, "envMap", envMapMips, 1);

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

				commands->ImageMemoryBarrier(commandList, envCubemap, envCubemap->GetFormat(), EImageLayout::ComputeWrite, envCubemap->GetDefaultLayout());
			}
			commands->EndDebugRegion(commandList);
		}

		if (bShouldUpdateIrradianceCubemap)
		{
			irradianceCubemap = RHI::Renderer::GetDriver()->CreateCubemap(ivec2(IrradianceMapSize, IrradianceMapSize),
				1,
				RHI::EFormat::R16G16B16A16_SFLOAT,
				RHI::ETextureFiltration::Linear,
				RHI::ETextureClamping::Clamp,
				usage);

			frameGraph->SetSampler("g_irradianceCubemap", irradianceCubemap);
			RHI::Renderer::GetDriver()->SetDebugName(irradianceCubemap, "g_irradianceCubemap");

			commands->ImageMemoryBarrier(commandList, irradianceCubemap, irradianceCubemap->GetFormat(), irradianceCubemap->GetDefaultLayout(), EImageLayout::General);

			commands->BeginDebugRegion(commandList, "Compute diffuse irradiance cubemap", DebugContext::Color_CmdCompute);
			{
				commands->ImageMemoryBarrier(commandList, envCubemap, envCubemap->GetFormat(), envCubemap->GetDefaultLayout(), EImageLayout::ShaderReadOnlyOptimal);
				commands->ImageMemoryBarrier(commandList, irradianceCubemap, irradianceCubemap->GetFormat(), irradianceCubemap->GetDefaultLayout(), EImageLayout::ComputeWrite);

				driver->AddSamplerToShaderBindings(m_computeIrradianceBindings, "envMap", envCubemap, 0);
				driver->AddStorageImageToShaderBindings(m_computeIrradianceBindings, "irradianceMap", irradianceCubemap, 1);

				m_computeIrradianceBindings->RecalculateCompatibility();

				commands->Dispatch(commandList,
					m_pComputeIrradianceShader->GetComputeShaderRHI(),
					IrradianceMapSize / 32u,
					IrradianceMapSize / 32u,
					6u,
					{ m_computeIrradianceBindings });

				//commands->ImageMemoryBarrier(commandList, envCubemap, envCubemap->GetFormat(), EImageLayout::ShaderReadOnlyOptimal, envCubemap->GetDefaultLayout());
				commands->ImageMemoryBarrier(commandList, irradianceCubemap, irradianceCubemap->GetFormat(), EImageLayout::ComputeWrite, EImageLayout::ShaderReadOnlyOptimal);
			}
			commands->EndDebugRegion(commandList);
		}

		m_bIsDirty = false;
	}

	commands->EndDebugRegion(commandList);
}

void EnvironmentNode::Clear()
{
}
