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

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* EnvironmentNode::m_name = "Environment";
#endif

void EnvironmentNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdCompute);

	if (!m_pComputeBrdfShader)
	{
		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeBrdfLut.shader"))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderInfo->GetUID(), m_pComputeBrdfShader);
		}

		m_computeBrdfBindings = driver->CreateShaderBindings();
	}

	if (!m_pComputeSpecularShader)
	{
		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeEnvMap_IBL.shader"))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderInfo->GetUID(), m_pComputeSpecularShader);
		}

		m_computeSpecularBindings = driver->CreateShaderBindings();
	}

	if (!m_irradianceCubemap)
	{
		if (auto envMap = frameGraph->GetSampler("g_environmentSampler"))
		{
			// Create all textures/cubemaps
			const RHI::ETextureUsageFlags usage = RHI::ETextureUsageBit::ColorAttachment_Bit |
				RHI::ETextureUsageBit::TextureTransferSrc_Bit |
				RHI::ETextureUsageBit::TextureTransferDst_Bit |
				RHI::ETextureUsageBit::Storage_Bit |
				RHI::ETextureUsageBit::Sampled_Bit;

			m_envCubemap = RHI::Renderer::GetDriver()->CreateCubemap(ivec2(EnvMapSize, EnvMapSize),
				EnvMapLevels,
				RHI::EFormat::R16G16B16A16_SFLOAT,
				RHI::ETextureFiltration::Linear,
				RHI::ETextureClamping::Clamp,
				usage);

			m_irradianceCubemap = RHI::Renderer::GetDriver()->CreateCubemap(ivec2(IrradianceMapSize, IrradianceMapSize),
				1,
				RHI::EFormat::R16G16B16A16_SFLOAT,
				RHI::ETextureFiltration::Linear,
				RHI::ETextureClamping::Clamp,
				usage);

			m_brdfSampler = RHI::Renderer::GetDriver()->CreateRenderTarget(ivec2(BrdfLutSize, BrdfLutSize), 1,
				RHI::EFormat::R16G16_SFLOAT, RHI::ETextureFiltration::Linear,
				RHI::ETextureClamping::Clamp, usage);

			RHI::Renderer::GetDriver()->SetDebugName(m_envCubemap, "g_envCubemap");
			RHI::Renderer::GetDriver()->SetDebugName(m_brdfSampler, "g_brdfSampler");
			RHI::Renderer::GetDriver()->SetDebugName(m_irradianceCubemap, "g_irradianceCubemap");

			frameGraph->SetSampler("g_envCubemap", m_envCubemap);
			frameGraph->SetSampler("g_irradianceCubemap", m_irradianceCubemap);
			frameGraph->SetSampler("g_brdfSampler", m_brdfSampler);

			RHI::RHICubemapPtr rawEnvCubemap = RHI::Renderer::GetDriver()->CreateCubemap(ivec2(EnvMapSize, EnvMapSize),
				EnvMapLevels,
				RHI::EFormat::R16G16B16A16_SFLOAT,
				RHI::ETextureFiltration::Linear,
				RHI::ETextureClamping::Clamp,
				usage);
			RHI::Renderer::GetDriver()->SetDebugName(rawEnvCubemap, "rawEnvCubemap");

			commands->BeginDebugRegion(commandList, "Generate Raw Env Cubemap from Equirect", DebugContext::Color_CmdCompute);
			{
				commands->ImageMemoryBarrier(commandList, rawEnvCubemap, rawEnvCubemap->GetFormat(), rawEnvCubemap->GetDefaultLayout(), EImageLayout::ComputeWrite);
				commands->ConvertEquirect2Cubemap(commandList, envMap, rawEnvCubemap);
				commands->ImageMemoryBarrier(commandList, rawEnvCubemap, rawEnvCubemap->GetFormat(), EImageLayout::ComputeWrite, EImageLayout::TransferDstOptimal);

				commands->GenerateMipMaps(commandList, rawEnvCubemap);
			}
			commands->EndDebugRegion(commandList);

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

				commands->ImageMemoryBarrier(commandList, m_brdfSampler, m_brdfSampler->GetFormat(), EImageLayout::ComputeWrite, m_brdfSampler->GetDefaultLayout());
			}
			commands->EndDebugRegion(commandList);

			commands->BeginDebugRegion(commandList, "Compute pre-filtered specular environment map", DebugContext::Color_CmdCompute);
			{
				struct PushConstants { int32_t level{}; float roughness; };
				const uint32_t NumMipTailLevels = EnvMapLevels - 1;

				commands->ImageMemoryBarrier(commandList, rawEnvCubemap, rawEnvCubemap->GetFormat(), rawEnvCubemap->GetDefaultLayout(), EImageLayout::TransferSrcOptimal);
				commands->ImageMemoryBarrier(commandList, m_envCubemap, m_envCubemap->GetFormat(), m_envCubemap->GetDefaultLayout(), EImageLayout::TransferDstOptimal);

				commands->BlitImage(commandList, rawEnvCubemap, m_envCubemap,
					glm::ivec4(0, 0, rawEnvCubemap->GetExtent().x, rawEnvCubemap->GetExtent().y),
					glm::ivec4(0, 0, m_envCubemap->GetExtent().x, m_envCubemap->GetExtent().y));

				commands->ImageMemoryBarrier(commandList, rawEnvCubemap, rawEnvCubemap->GetFormat(), EImageLayout::TransferSrcOptimal, EImageLayout::ShaderReadOnlyOptimal);
				commands->ImageMemoryBarrier(commandList, m_envCubemap, m_envCubemap->GetFormat(), EImageLayout::TransferDstOptimal, EImageLayout::ComputeWrite);
				
				// Pre-filter rest of the mip-chain.
				TVector<RHI::RHITexturePtr> envMapMips;
				for (uint32_t level = 1; level < EnvMapLevels; ++level)
				{
					envMapMips.Add(m_envCubemap->GetMipLevel(level));
				}

				driver->AddSamplerToShaderBindings(m_computeSpecularBindings, "rawEnvMap", rawEnvCubemap, 0);
				driver->AddStorageImageToShaderBindings(m_computeSpecularBindings, "envMap", envMapMips, 1);


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

				commands->ImageMemoryBarrier(commandList, m_envCubemap, m_envCubemap->GetFormat(), EImageLayout::ComputeWrite, m_envCubemap->GetDefaultLayout());
			}
			commands->EndDebugRegion(commandList);
		}
	}

	commands->EndDebugRegion(commandList);
}

void EnvironmentNode::Clear()
{
}
