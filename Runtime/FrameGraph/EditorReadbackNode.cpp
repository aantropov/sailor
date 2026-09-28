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
	if (!App::HasEditor() || !sceneView.m_submissionContext)
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

	uint32_t bytesPerPixel;
	switch (m_texture->GetFormat())
	{
	case ETextureFormat::B8G8R8A8_UNORM:
	case ETextureFormat::B8G8R8A8_SRGB:
	case ETextureFormat::R8G8B8A8_UNORM:
	case ETextureFormat::R8G8B8A8_SRGB: bytesPerPixel = 4; break;
	case ETextureFormat::R16G16B16A16_SFLOAT: bytesPerPixel = 8; break;
	default: return;
	}

	if (m_readbacks.IsEmpty()) m_readbacks.Resize(driver->GetMaxFramesInFlight() + 1u);
	TSharedPtr<EditorReadbackFrame>* slot = nullptr;
	for (auto& candidate : m_readbacks)
	{
		if (!candidate || (!candidate.IsShared() && candidate->m_completion->GetStatus() != EFenceStatus::Pending))
		{
			slot = &candidate;
			break;
		}
	}
	if (!slot) return;

	if (!*slot) *slot = TSharedPtr<EditorReadbackFrame>::Make();
	auto& readback = **slot;
	const auto extent = m_texture->GetExtent();
	const uint32_t bytesPerRow = static_cast<uint32_t>(extent.x) * bytesPerPixel;
	const size_t requiredSize = static_cast<size_t>(bytesPerRow) * extent.y;
	if (!readback.m_buffer || readback.m_buffer->GetSize() < requiredSize)
	{
		readback.m_buffer = driver->CreateBuffer(requiredSize, EBufferUsageBit::BufferTransferDst_Bit,
			EMemoryPropertyBit::HostCoherent | EMemoryPropertyBit::HostVisible);
	}
	if (!readback.m_buffer)
	{
		slot->Clear();
		return;
	}
	readback.m_extent = extent;
	readback.m_bytesPerRow = bytesPerRow;
	readback.m_format = m_texture->GetFormat();
	readback.m_frameIndex = m_nextFrameIndex++;
	readback.m_generation = sceneView.m_submissionContext->GetResourceGeneration();
	readback.m_completion = sceneView.m_submissionContext->GetOrCreateFrameCompletion();

	commands->BeginDebugRegion(commandList, GetName(), glm::vec4(0.8f, 0.3f, 0.2f, 1.0f));
	commands->ImageMemoryBarrier(commandList, m_texture, EImageLayout::TransferSrcOptimal);
	commands->CopyImageToBuffer(commandList, m_texture, readback.m_buffer);
	commands->MemoryBarrier(commandList, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
		static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
	commands->EndDebugRegion(commandList);
}

EditorReadbackFramePtr EditorReadbackNode::TakeCompletedFrame()
{
	EditorReadbackFramePtr latest;
	for (const auto& frame : m_readbacks)
	{
		if (frame && frame->m_frameIndex > m_publishedFrameIndex &&
			frame->m_completion->GetStatus() == EFenceStatus::Finished &&
			(!latest || frame->m_frameIndex > latest->m_frameIndex))
		{
			latest = frame;
		}
	}
	if (latest) m_publishedFrameIndex = latest->m_frameIndex;
	return latest;
}

void EditorReadbackNode::Clear()
{
	m_readbacks.Clear();
	m_texture.Clear();
}
