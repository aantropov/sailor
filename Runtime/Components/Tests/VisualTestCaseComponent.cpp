#include "Components/Tests/VisualTestCaseComponent.h"
#include "FrameGraph/CPUPathTracerNode.h"
#include "FrameGraph/CopyTextureToRamNode.h"
#include "RHI/Renderer.h"
#include "Platform/Time.h"

using namespace Sailor;

void VisualTestCaseComponent::BeginPlay()
{
	TestCaseComponent::BeginPlay();
	m_startTimeMs = Utils::GetCurrentTimeMs();
	m_framesSinceStart = 0;
	m_capture.Clear();

	m_bPendingPathTracerConfig = m_bEnablePathTracer && !ConfigurePathTracer();
}

void VisualTestCaseComponent::Tick(float deltaTime)
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

	if (!m_capture)
	{
		const int64_t elapsedMs = Utils::GetCurrentTimeMs() - m_startTimeMs;
		if (m_framesSinceStart < m_captureAfterFrames || elapsedMs < (int64_t)(m_minRunTimeSeconds * 1000.0f))
		{
			return;
		}

		auto snapshotNode = renderer->GetFrameGraph()->GetRHI()->GetGraphNode("CopyTextureToRam"_h);
		auto snapshot = snapshotNode.DynamicCast<Framegraph::CopyTextureToRamNode>();
		if (!snapshot)
		{
			MarkFailed("CopyTextureToRam node was not found.");
			return;
		}

		m_capture = snapshot->DoOneCapture();
		m_captureRequestedAtMs = Utils::GetCurrentTimeMs();
		return;
	}

	if (!m_capture->IsFinished())
	{
		if (Utils::GetCurrentTimeMs() - m_captureRequestedAtMs > 30000)
			MarkFailed("Timed out waiting for capture completion.");
		return;
	}

	const auto& frame = m_capture->GetResult();
	if (!frame)
	{
		MarkFailed("Capture was cancelled or its GPU submission failed.");
		return;
	}
	std::string error;
	if (CaptureScreenshot(*frame, m_screenshotName, error))
	{
		AddJournalEvent("CaptureSmoke", "Completed readback saved; image correctness was not evaluated.");
		MarkPassed();
		return;
	}
	MarkFailed(error);
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

	Tasks::CreateTask("Configure visual-test path tracer"_h,
		[node = cpuPathTracerNode, bIsEnabled = m_bEnablePathTracerNode, samples = m_samplesPerFrame,
			bounces = m_maxBounces, blend = m_blend, biasBase = m_rayBiasBase, biasScale = m_rayBiasScale]() mutable
		{
			node->SetFloat("enabled"_h, bIsEnabled ? 1.0f : 0.0f);
			node->SetFloat("samplesPerFrame"_h, samples);
			node->SetFloat("maxBounces"_h, bounces);
			node->SetFloat("blend"_h, blend);
			node->SetFloat("rayBiasBase"_h, biasBase);
			node->SetFloat("rayBiasScale"_h, biasScale);
		}, EThreadType::Render)->Run();

	return true;
}
