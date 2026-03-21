#include "Components/Tests/VisualTestCaseComponent.h"
#include "FrameGraph/CPUPathTracerNode.h"
#include "FrameGraph/CopyTextureToRamNode.h"
#include "RHI/Renderer.h"

using namespace Sailor;

namespace
{
	constexpr uint32_t ScreenshotReadbackGraceFrames = 16u;
}

void VisualTestCaseComponent::BeginPlay()
{
	TestCaseComponent::BeginPlay();

	if (m_bEnablePathTracer)
	{
		m_bPendingPathTracerConfig = !ConfigurePathTracer();
	}
}

void VisualTestCaseComponent::Tick(float)
{
	if (IsFinished())
	{
		return;
	}

	if (m_bPendingPathTracerConfig)
	{
		m_bPendingPathTracerConfig = !ConfigurePathTracer();
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

	if (m_framesAfterCaptureRequest < ScreenshotReadbackGraceFrames)
	{
		return;
	}

	std::string error;
	if (CaptureScreenshot(m_screenshotName, error))
	{
		MarkPassed();
		return;
	}

	if (m_framesAfterCaptureRequest > ScreenshotReadbackGraceFrames + 104u)
	{
		MarkFailed(error.empty() ? "Timed out waiting for screenshot readback." : error);
	}
}

bool VisualTestCaseComponent::ConfigurePathTracer()
{
	auto renderer = App::GetSubmodule<RHI::Renderer>();
	if (!renderer || !renderer->GetFrameGraph())
	{
		return false;
	}

	auto cpuPathTracerNode = renderer->GetFrameGraph()->GetRHI()->GetGraphNode(Framegraph::CPUPathTracerNode::GetName()).DynamicCast<Framegraph::CPUPathTracerNode>();
	if (!cpuPathTracerNode)
	{
		return false;
	}

	cpuPathTracerNode->SetFloat("enabled", m_bEnablePathTracerNode ? 1.0f : 0.0f);
	cpuPathTracerNode->SetFloat("samplesPerFrame", m_samplesPerFrame);
	cpuPathTracerNode->SetFloat("maxBounces", m_maxBounces);
	cpuPathTracerNode->SetFloat("blend", m_blend);
	cpuPathTracerNode->SetFloat("rayBiasBase", m_rayBiasBase);
	cpuPathTracerNode->SetFloat("rayBiasScale", m_rayBiasScale);

	return true;
}
