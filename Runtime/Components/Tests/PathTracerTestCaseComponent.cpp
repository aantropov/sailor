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
	if (!m_bCaptureRequested)
	{
		if (m_framesSinceStart < m_captureAfterFrames)
		{
			return;
		}

		m_bCaptureRequested = true;
		m_framesAfterCaptureRequest = 0;
	}

	std::string error;
	if (CaptureOutput(error))
	{
		MarkPassed();
		return;
	}

	m_framesAfterCaptureRequest++;
	if (m_framesAfterCaptureRequest > m_timeoutFrames)
	{
		MarkFailed(error.empty() ? "Path tracer output is not ready." : error);
	}
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

	cpuPathTracer->SetFloat("enabled", m_bEnableNode ? 1.0f : 0.0f);
	cpuPathTracer->SetFloat("samplesPerFrame", m_samplesPerFrame);
	cpuPathTracer->SetFloat("maxBounces", m_maxBounces);
	cpuPathTracer->SetFloat("blend", m_blend);
	cpuPathTracer->SetFloat("rayBiasBase", m_rayBiasBase);
	cpuPathTracer->SetFloat("rayBiasScale", m_rayBiasScale);
	return true;
}

bool PathTracerTestCaseComponent::CaptureOutput(std::string& outError) const
{
	auto renderer = App::GetSubmodule<RHI::Renderer>();
	if (!renderer || !renderer->GetFrameGraph())
	{
		outError = "Renderer frame graph is not available.";
		return false;
	}

	auto cpuPathTracerNode = renderer->GetFrameGraph()->GetRHI()->GetGraphNode(Framegraph::CPUPathTracerNode::GetName()).DynamicCast<Framegraph::CPUPathTracerNode>();
	if (!cpuPathTracerNode)
	{
		outError = "CPUPathTracerNode is not available.";
		return false;
	}

	TVector<glm::u8vec4> image;
	glm::uvec2 extent{};
	if (!cpuPathTracerNode->GetLastRenderedImage(image, extent))
	{
		outError = "Path tracer output is not ready yet.";
		return false;
	}

	if (image.Num() < (size_t)extent.x * (size_t)extent.y)
	{
		outError = "Path tracer output is incomplete.";
		return false;
	}

	return TestCaseComponent::SaveImageToPng(image, extent, m_outputName, outError);
}
