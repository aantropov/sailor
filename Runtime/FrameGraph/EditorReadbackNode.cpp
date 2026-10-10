#include "EditorReadbackNode.h"
#include "RHI/Buffer.h"
#include "RHI/Fence.h"
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

namespace
{
	uint32_t GetReadbackPixelSize(const RHITexturePtr& texture)
	{
		return texture && texture->GetMsaaSamples() == EMsaaSamples::Samples_1 ?
			RHI::GetReadbackPixelSize(texture->GetFormat()) : 0u;
	}
}

TRefPtr<EditorReadbackNode> EditorReadbackNode::Find(RHIFrameGraph& graph)
{
	if (auto primary = graph.GetGraphNode(GetName()).DynamicCast<EditorReadbackNode>()) return primary;
	for (auto& node : graph.GetGraph())
	{
		if (auto readback = node.DynamicCast<EditorReadbackNode>()) return readback;
	}
	return {};
}

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

	m_texture = GetResolvedAttachment("src"_h);
	if (!m_resourceParams.ContainsKey("src"_h) && frameGraph)
	{
		auto resolve = [&](StringHash name) -> RHITexturePtr
			{
				if (auto surface = frameGraph->GetSurface(name))
				{
					auto texture = surface->GetResolved() ? surface->GetResolved() : surface->GetTarget();
					if (GetReadbackPixelSize(texture)) return texture;
				}
				if (auto target = frameGraph->GetRenderTarget(name)) return target;
				return frameGraph->GetSampler(name);
			};
		if (m_unresolvedResourceParams.ContainsKey("src"_h))
		{
			m_texture = resolve(m_unresolvedResourceParams["src"_h]);
		}
		else
		{
			// An omitted source follows the same priority as the editor viewport.
			for (const auto name : { "EditorOutput"_h, "Main"_h, "BackBuffer"_h, "Secondary"_h })
			{
				m_texture = resolve(name);
				if (GetReadbackPixelSize(m_texture)) break;
			}
		}
	}

	const uint32_t bytesPerPixel = GetReadbackPixelSize(m_texture);
	if (!bytesPerPixel) return;

	if (m_readbacks.IsEmpty()) m_readbacks.Resize(driver->GetMaxFramesInFlight() + 1u);
	TSharedPtr<ReadbackFrame>* slot = nullptr;
	for (auto& candidate : m_readbacks)
	{
		if (!candidate || (!candidate.IsShared() && candidate->m_completion->GetStatus() != EFenceStatus::Pending))
		{
			slot = &candidate;
			break;
		}
	}
	if (!slot) return;

	if (!*slot) *slot = TSharedPtr<ReadbackFrame>::Make();
	auto& readback = **slot;
	const auto extent = m_texture->GetExtent();
	const uint32_t bytesPerRow = static_cast<uint32_t>(extent.x) * bytesPerPixel;
	const size_t requiredSize = static_cast<size_t>(bytesPerRow) * extent.y;
	if (!readback.m_buffer || readback.m_buffer->GetSize() < requiredSize)
	{
		readback.m_buffer = driver->CreateBuffer(requiredSize, EBufferUsageBit::BufferTransferDst_Bit,
			EMemoryPropertyBit::HostCoherent | EMemoryPropertyBit::HostVisible);
		if (readback.m_buffer) m_stats.m_bufferAllocatedBytes += requiredSize;
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
	m_stats.m_recordedReadbackBytes += requiredSize;
	commands->MemoryBarrier(commandList, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
		static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
	commands->EndDebugRegion(commandList);
}

ReadbackFramePtr EditorReadbackNode::TakeCompletedFrame()
{
	TSharedPtr<ReadbackFrame> latest;
	for (const auto& frame : m_readbacks)
	{
		if (frame && frame->m_frameIndex > m_publishedFrameIndex &&
			frame->m_completion->GetStatus() == EFenceStatus::Finished &&
			(!latest || frame->m_frameIndex > latest->m_frameIndex))
		{
			latest = frame;
		}
	}
	if (latest)
	{
		const size_t capacity = latest->m_bgraPixels.Capacity();
		if (!latest->PrepareBgraPixels()) return {};
		if (latest->m_bgraPixels.Capacity() > capacity) m_stats.m_conversionAllocatedBytes += latest->m_bgraPixels.Capacity();
		m_stats.m_convertedBytes += latest->m_bgraPixels.Num();
		m_publishedFrameIndex = latest->m_frameIndex;
	}
	return latest;
}

void EditorReadbackNode::Clear()
{
	m_readbacks.Clear();
	m_texture.Clear();
}
