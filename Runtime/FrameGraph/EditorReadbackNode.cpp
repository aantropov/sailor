#include "EditorReadbackNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Surface.h"
#include "RHI/RenderTarget.h"
#include "RHI/Texture.h"
#include "Engine/World.h"
#include "Engine/GameObject.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* EditorReadbackNode::m_name = "EditorReadback";
#endif

void EditorReadbackNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	(void)sceneView;

	if (!App::HasEditor())
	{
		return;
	}

	SAILOR_PROFILE_FUNCTION();
	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	if (!driver || !commands || !commandList)
	{
		return;
	}

	m_texture = GetResolvedAttachment("src");
	if (!m_texture)
	{
		return;
	}

	const uint64_t requiredSize = m_texture->GetSize();
	if (!m_cpuBuffer || m_cpuBuffer->GetSize() < requiredSize)
	{
		m_cpuBuffer = driver->CreateBuffer(requiredSize, RHI::EBufferUsageBit::BufferTransferDst_Bit,
			RHI::EMemoryPropertyBit::HostCoherent | RHI::EMemoryPropertyBit::HostVisible);
	}

	m_bytesPerRow = static_cast<uint32_t>(m_texture->GetExtent().x) * 4u;

	commands->BeginDebugRegion(commandList, GetName(), glm::vec4(0.8f, 0.3f, 0.2f, 1.0f));
	commands->ImageMemoryBarrier(commandList, m_texture, EImageLayout::TransferSrcOptimal);
	commands->CopyImageToBuffer(commandList, m_texture, m_cpuBuffer);
	commands->EndDebugRegion(commandList);
}

void EditorReadbackNode::Clear()
{
}
