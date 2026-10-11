#include "MotionBlurNode.h"
#include "RHI/SceneView.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

namespace
{
	const bool bRegistered = []
	{
		FrameGraphBuilder::RegisterFrameGraphNode(MotionBlurNode::GetName(), [] { return TRefPtr<MotionBlurNode>::Make(); });
		return true;
	}();
}

MotionBlurNode::MotionBlurNode()
{
	SetString("shader"_h, "Shaders/MotionBlur.shader");
}

Tasks::TaskPtr<> MotionBlurNode::Prepare(RHIFrameGraphPtr frameGraph, RHISceneViewSnapshot& sceneView)
{
	PreloadShader();
	if (m_history.Num() <= sceneView.m_cameraIndex)
	{
		m_history.Resize(sceneView.m_cameraIndex + 1);
	}
	auto& history = m_history[sceneView.m_cameraIndex];
	auto current = TSharedPtr<const RHIMotionHistoryFrame>::Make(
		CaptureMotionHistory(sceneView, frameGraph->GetSceneRenderExtent()));
	sceneView.m_previousMotionFrame.Clear();
	if (history.m_frame && history.m_completion->IsSuccessful() &&
		IsMotionHistoryContinuous(*history.m_frame, *current))
	{
		sceneView.m_previousMotionFrame = history.m_frame;
	}
	history = { std::move(current), sceneView.m_submissionCompletionToken };
	return {};
}

void MotionBlurNode::Process(RHIFrameGraphPtr frameGraph, RHICommandListPtr transferCommandList,
	RHICommandListPtr commandList, const RHISceneViewSnapshot& sceneView)
{
	UploadMotionData(transferCommandList, sceneView, frameGraph->GetSceneRenderExtent());
	PostProcessNode::Process(frameGraph, transferCommandList, commandList, sceneView);
}

void MotionBlurNode::Clear()
{
	PostProcessNode::Clear();
	m_history.Clear();
}
