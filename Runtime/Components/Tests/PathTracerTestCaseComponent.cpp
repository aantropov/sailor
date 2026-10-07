#include "Components/Tests/PathTracerTestCaseComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Components/PathTracerProxyComponent.h"
#include "FrameGraph/CPUPathTracerNode.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "RHI/Renderer.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"

using namespace Sailor;

void PathTracerTestCaseComponent::BeginPlay()
{
	TestCaseComponent::BeginPlay();
	m_capture.Clear();
	m_framesSinceStart = 0;
	m_framesAfterCaptureRequest = 0;

	m_bProxiesAttached = AttachProxies();
	m_bPendingPathTracerConfig = !ConfigurePathTracer();
}

void PathTracerTestCaseComponent::Tick(float)
{
	if (IsFinished())
	{
		return;
	}

	if (m_bAttachToAllMeshes && !m_bProxiesAttached)
	{
		m_bProxiesAttached = AttachProxies();
	}

	if (m_bPendingPathTracerConfig)
	{
		m_bPendingPathTracerConfig = !ConfigurePathTracer();
	}

	m_framesSinceStart++;
	if (!m_capture)
	{
		if (m_framesSinceStart < m_captureAfterFrames)
		{
			return;
		}

		auto* renderer = App::GetSubmodule<RHI::Renderer>();
		auto graph = renderer ? renderer->GetFrameGraph() : FrameGraphPtr{};
		auto node = graph ? graph->GetRHI()->GetGraphNode(Framegraph::CPUPathTracerNode::GetName()).DynamicCast<Framegraph::CPUPathTracerNode>() : nullptr;
		if (!node)
		{
			MarkFailed("CPUPathTracerNode is not available.");
			return;
		}
		m_capture = node->DoOneCapture();
		m_framesAfterCaptureRequest = 0;
		return;
	}

	if (!m_capture->IsFinished())
	{
		if (++m_framesAfterCaptureRequest > m_timeoutFrames)
			MarkFailed("Timed out waiting for the requested path tracer image.");
		return;
	}
	const auto& image = m_capture->GetResult();
	if (!image)
	{
		MarkFailed("Path tracer capture was cancelled or the requested camera produced no image.");
		return;
	}
	TVector<glm::u8vec4> pixels(image->m_pixels.Num());
	for (size_t i = 0; i < pixels.Num(); ++i) pixels[i] = Utils::LinearToSRGB8(image->m_pixels[i]);
	std::string error;
	if (SaveImageToPng(pixels, image->m_extent, m_outputName, error))
	{
		AddJournalEvent("CaptureSmoke", "Requested CPU image saved; image correctness was not evaluated.");
		MarkPassed();
	}
	else MarkFailed(error);
}

bool PathTracerTestCaseComponent::AttachProxies()
{
	if (m_bProxiesAttached || !m_bAttachToAllMeshes)
	{
		return m_bProxiesAttached;
	}

	for (auto& go : GetWorld()->GetGameObjects())
	{
		auto meshRenderer = go->GetComponent<MeshRendererComponent>();
		if (!meshRenderer)
		{
			continue;
		}

		auto proxy = go->GetComponent<PathTracerProxyComponent>();
		if (!proxy)
		{
			proxy = go->AddComponent<PathTracerProxyComponent>();
		}

		proxy->SetEnabled(true);
		proxy->SetRebuildEveryFrame(m_bRebuildEveryFrame);
	}

	m_bProxiesAttached = true;
	return true;
}

bool PathTracerTestCaseComponent::ConfigurePathTracer()
{
	auto renderer = App::GetSubmodule<RHI::Renderer>();
	if (!renderer)
	{
		return false;
	}

	auto frameGraph = renderer->GetFrameGraph();
	auto rhiFrameGraph = frameGraph ? frameGraph->GetRHI() : nullptr;
	auto cpuPathTracer = rhiFrameGraph ? rhiFrameGraph->GetGraphNode(Framegraph::CPUPathTracerNode::GetName()).DynamicCast<Framegraph::CPUPathTracerNode>() : nullptr;
	if (!cpuPathTracer)
	{
		return false;
	}

	Tasks::CreateTask("Configure CPU path tracer"_h,
		[node = cpuPathTracer, bIsEnabled = m_bEnableNode, samples = m_samplesPerFrame,
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
