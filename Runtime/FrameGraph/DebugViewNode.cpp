#include "DebugViewNode.h"

#include "RHI/RenderDebugView.h"
#include "RHI/SceneView.h"

using namespace Sailor;
using namespace Sailor::Framegraph;
using namespace Sailor::RHI;

#ifndef _SAILOR_IMPORT_
const char* DebugViewNode::m_name = "DebugView";
#endif

Tasks::TaskPtr<void, void> DebugViewNode::Prepare(
	RHIFrameGraphPtr,
	const RHISceneViewSnapshot&)
{
	EnsurePasses();
	for (auto& pass : m_debugPasses)
	{
		pass->PreloadShader();
	}
	return {};
}

void DebugViewNode::Process(
	RHIFrameGraphPtr frameGraph,
	RHICommandListPtr transferCommandList,
	RHICommandListPtr commandList,
	const RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	ResetDrawCallStats();
	EnsurePasses();

	const ESceneViewRenderMode mode = sceneView.m_renderMode;
	PostProcessNode* debugPass = GetDebugPass(mode);
	if (debugPass && debugPass->IsShaderReady())
	{
		debugPass->Process(
			frameGraph,
			transferCommandList,
			commandList,
			sceneView);
		m_drawCallStats = debugPass->GetDrawCallStats();
		return;
	}

	m_litPass->Process(
		frameGraph,
		transferCommandList,
		commandList,
		sceneView);
	m_drawCallStats = m_litPass->GetDrawCallStats();
}

void DebugViewNode::Clear()
{
	if (m_litPass)
	{
		m_litPass->Clear();
	}
	m_litPass.Clear();
	for (auto& pass : m_debugPasses)
	{
		if (pass)
		{
			pass->Clear();
		}
		pass.Clear();
	}
}

void DebugViewNode::EnsurePasses()
{
	if (m_litPass)
	{
		return;
	}

	m_litPass = TRefPtr<BlitNode>::Make();
	CopyResource(*m_litPass, "src", "src");
	CopyResource(*m_litPass, "dst", "dst");

	constexpr std::array modes{
		ESceneViewRenderMode::AmbientOcclusion,
		ESceneViewRenderMode::Cascades,
		ESceneViewRenderMode::LightTiles
	};
	for (size_t index = 0u; index < modes.size(); ++index)
	{
		auto& pass = m_debugPasses[index];
		pass = TRefPtr<PostProcessNode>::Make();
		pass->SetString("shader", GetString("shader"));
		pass->SetString(
			"defines",
			GetSceneViewRenderModeShaderDefine(modes[index]));
		CopyResource(*pass, "color", "dst");
		CopyResource(*pass, "ldrSceneSampler", "src");
		CopyResource(*pass, "linearDepthSampler", "linearDepth");
	}
}

void DebugViewNode::CopyResource(
	BaseFrameGraphNode& destination,
	const std::string& destinationName,
	const std::string& sourceName)
{
	if (m_resourceParams.ContainsKey(sourceName))
	{
		destination.SetRHIResource(
			destinationName,
			GetRHIResource(sourceName));
	}
	else if (m_unresolvedResourceParams.ContainsKey(sourceName))
	{
		destination.SetRHIResource_Unresolved(
			destinationName,
			m_unresolvedResourceParams[sourceName]);
	}
}

PostProcessNode* DebugViewNode::GetDebugPass(ESceneViewRenderMode mode)
{
	switch (mode)
	{
	case ESceneViewRenderMode::AmbientOcclusion:
		return m_debugPasses[0].GetRawPtr();
	case ESceneViewRenderMode::Cascades:
		return m_debugPasses[1].GetRawPtr();
	case ESceneViewRenderMode::LightTiles:
		return m_debugPasses[2].GetRawPtr();
	case ESceneViewRenderMode::Lit:
	default:
		return nullptr;
	}
}
