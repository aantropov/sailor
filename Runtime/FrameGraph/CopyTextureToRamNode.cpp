#include "CopyTextureToRamNode.h"
#include "RHI/Buffer.h"
#include "RHI/Fence.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Texture.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

CopyTextureToRamNode::~CopyTextureToRamNode()
{
	Clear();
}

CopyTextureToRamNode::CaptureTask CopyTextureToRamNode::DoOneCapture()
{
	auto result = Tasks::CreateTask<ReadbackFramePtr, ReadbackFramePtr>("Texture capture result"_h,
		[](ReadbackFramePtr frame) { return frame; }, EThreadType::Main);
	Tasks::CreateTask("Request texture capture"_h, [self = ToRefPtr<CopyTextureToRamNode>(), result]() mutable
		{
			self->m_requests.Add(result);
		}, EThreadType::Render)->Run();
	return result;
}

void CopyTextureToRamNode::PollCaptures()
{
	for (size_t i = 0; i < m_captures.Num();)
	{
		auto& capture = m_captures[i];
		const auto status = capture.m_frame->m_completion->GetStatus();
		if (status == EFenceStatus::Pending) { ++i; continue; }
		for (auto& request : capture.m_requests)
		{
			request->SetArgs(status == EFenceStatus::Finished ? ReadbackFramePtr(capture.m_frame) : ReadbackFramePtr{});
			request->Run();
		}
		m_captures.RemoveAt(i);
	}
}

void CopyTextureToRamNode::Process(RHIFrameGraphPtr frameGraph, RHICommandListPtr,
	RHICommandListPtr commandList, const RHISceneViewSnapshot& sceneView)
{
	PollCaptures();
	if (m_requests.IsEmpty()) return;
	SAILOR_PROFILE_FUNCTION();
	const auto texture = GetResolvedAttachment("src"_h, frameGraph.GetRawPtr());
	const auto pixelSize = texture ? GetReadbackPixelSize(texture->GetFormat()) : 0u;
	if (!pixelSize || texture->GetMsaaSamples() != EMsaaSamples::Samples_1 || !sceneView.m_submissionContext)
	{
		for (auto& request : m_requests) request->Run();
		m_requests.Clear();
		return;
	}

	auto frame = TSharedPtr<ReadbackFrame>::Make();
	frame->m_extent = texture->GetExtent();
	frame->m_format = texture->GetFormat();
	frame->m_bytesPerRow = static_cast<uint32_t>(frame->m_extent.x) * pixelSize;
	frame->m_frameIndex = sceneView.m_submissionContext->GetSubmissionId();
	frame->m_generation = sceneView.m_submissionContext->GetResourceGeneration();
	frame->m_buffer = Renderer::GetDriver()->CreateBuffer(static_cast<size_t>(frame->m_bytesPerRow) * frame->m_extent.y,
		EBufferUsageBit::BufferTransferDst_Bit, EMemoryPropertyBit::HostCoherent | EMemoryPropertyBit::HostVisible);
	if (!frame->m_buffer)
	{
		for (auto& request : m_requests) request->Run();
		m_requests.Clear();
		return;
	}
	frame->m_completion = sceneView.m_submissionContext->GetOrCreateFrameCompletion();

	auto commands = Renderer::GetDriverCommands();
	commands->BeginDebugRegion(commandList, GetName(), glm::vec4(1.0f));
	commands->ImageMemoryBarrier(commandList, texture, EImageLayout::TransferSrcOptimal);
	commands->CopyImageToBuffer(commandList, texture, frame->m_buffer);
	commands->MemoryBarrier(commandList, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
		static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
	commands->EndDebugRegion(commandList);
	m_captures.Add(Capture{ std::move(frame), std::move(m_requests) });
}

void CopyTextureToRamNode::Clear()
{
	for (auto& request : m_requests) request->Run();
	for (auto& capture : m_captures)
		for (auto& request : capture.m_requests) request->Run();
	m_requests.Clear();
	m_captures.Clear();
}
