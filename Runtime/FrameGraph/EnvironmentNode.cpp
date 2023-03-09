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
	//auto driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdTransfer);

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

			// Temp cubemap to generate data
			RHI::RHICubemapPtr rawEnvCubemap = RHI::Renderer::GetDriver()->CreateCubemap(ivec2(EnvMapSize, EnvMapSize),
				EnvMapLevels,
				RHI::EFormat::R16G16B16A16_SFLOAT,
				RHI::ETextureFiltration::Linear,
				RHI::ETextureClamping::Clamp,
				usage);
			RHI::Renderer::GetDriver()->SetDebugName(rawEnvCubemap, "rawEnvCubemap");

			commands->ImageMemoryBarrier(commandList, rawEnvCubemap, rawEnvCubemap->GetFormat(), rawEnvCubemap->GetDefaultLayout(), EImageLayout::ComputeWrite);
			commands->ConvertEquirect2Cubemap(commandList, envMap, rawEnvCubemap);
			commands->ImageMemoryBarrier(commandList, rawEnvCubemap, rawEnvCubemap->GetFormat(), EImageLayout::ComputeWrite, EImageLayout::TransferDstOptimal);

			commands->GenerateMipMaps(commandList, rawEnvCubemap);
			//

		}
	}

	commands->EndDebugRegion(commandList);
}

void EnvironmentNode::Clear()
{
}
