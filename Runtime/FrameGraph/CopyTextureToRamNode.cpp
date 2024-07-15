#include "CopyTextureToRamNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/RenderTarget.h"
#include "RHI/Texture.h"
#include "Engine/World.h"
#include "Engine/GameObject.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* CopyTextureToRamNode::m_name = "CopyTextureToRam";
#endif

void CopyTextureToRamNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	if (!m_captureThisFrame)
		return;

	SAILOR_PROFILE_FUNCTION();
	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	if ((m_texture = GetResolvedAttachment("src")))
	{
		commands->ImageMemoryBarrier(commandList, m_texture, EImageLayout::TransferSrcOptimal);
		if (!m_cpuBuffer || m_cpuBuffer->GetSize() < m_texture->GetSize())
		{
			m_cpuBuffer = driver->CreateBuffer(m_texture->GetSize() + 512u, RHI::EBufferUsageBit::BufferTransferDst_Bit,
				RHI::EMemoryPropertyBit::HostCoherent | RHI::EMemoryPropertyBit::HostVisible);
		}

		commands->BeginDebugRegion(commandList, GetName(), glm::vec4(1.0f));
		commands->CopyImageToBuffer(commandList, m_texture, m_cpuBuffer);
		commands->EndDebugRegion(commandList);
	}

	m_captureThisFrame = false;
}

void CopyTextureToRamNode::Clear()
{
}
