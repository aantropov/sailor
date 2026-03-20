#include "Components/Tests/VisualTestCaseComponent.h"
#include "FrameGraph/CopyTextureToRamNode.h"
#include "RHI/Renderer.h"

using namespace Sailor;

void VisualTestCaseComponent::Tick(float deltaTime)
{
	if (IsFinished())
	{
		return;
	}

	m_framesSinceStart++;

	auto renderer = App::GetSubmodule<RHI::Renderer>();
	if (!renderer || !renderer->GetFrameGraph())
	{
		MarkFailed("Renderer frame graph is not available.");
		return;
	}

	if (!m_bCaptureRequested)
	{
		if (m_framesSinceStart < m_captureAfterFrames)
		{
			return;
		}

		auto snapshotNode = renderer->GetFrameGraph()->GetRHI()->GetGraphNode("CopyTextureToRam");
		auto snapshot = snapshotNode.DynamicCast<Framegraph::CopyTextureToRamNode>();
		if (!snapshot)
		{
			MarkFailed("CopyTextureToRam node was not found.");
			return;
		}

		snapshot->DoOneCapture();
		m_bCaptureRequested = true;
		m_framesAfterCaptureRequest = 0;
		return;
	}

	m_framesAfterCaptureRequest++;

	if (m_framesAfterCaptureRequest < 16)
	{
		return;
	}

	std::string error;
	if (CaptureScreenshot(m_screenshotName, error))
	{
		MarkPassed();
		return;
	}

	if (m_framesAfterCaptureRequest > 120)
	{
		MarkFailed(error.empty() ? "Timed out waiting for screenshot readback." : error);
	}
}
