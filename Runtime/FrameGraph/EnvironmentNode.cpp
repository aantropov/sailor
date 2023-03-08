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

	//if (!m_irradianceCubemap)
	{
		if (auto envMap = frameGraph->GetSampler("g_environmentSampler"))
		{
			m_irradianceCubemap = RHI::Renderer::GetDriver()->CreateCubemap(envMap->GetExtent(), 1, RHI::EFormat::R16G16B16A16_SFLOAT);
			RHI::Renderer::GetDriver()->SetDebugName(m_irradianceCubemap, "g_irradianceCubemap");

			frameGraph->SetSampler("g_irradianceCubemap", m_irradianceCubemap);

			commands->ImageMemoryBarrier(commandList, envMap, envMap->GetFormat(), envMap->GetDefaultLayout(), EImageLayout::ComputeRead);
			commands->ImageMemoryBarrier(commandList, m_irradianceCubemap, m_irradianceCubemap->GetFormat(), m_irradianceCubemap->GetDefaultLayout(), EImageLayout::ComputeWrite);

			commands->ConvertEquirect2Cubemap(commandList, envMap, m_irradianceCubemap);

			commands->ImageMemoryBarrier(commandList, envMap, envMap->GetFormat(), EImageLayout::ComputeRead, envMap->GetDefaultLayout());
			commands->ImageMemoryBarrier(commandList, m_irradianceCubemap, m_irradianceCubemap->GetFormat(), EImageLayout::ComputeWrite, m_irradianceCubemap->GetDefaultLayout());
		}
	}

	commands->EndDebugRegion(commandList);
}

void EnvironmentNode::Clear()
{
}
