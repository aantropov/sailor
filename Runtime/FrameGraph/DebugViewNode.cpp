#include "DebugViewNode.h"

#include "RHI/RenderDebugView.h"
#include "RHI/SceneView.h"

using namespace Sailor;
using namespace Sailor::Framegraph;
using namespace Sailor::RHI;

Tasks::TaskPtr<void, void> DebugViewNode::Prepare(
	RHIFrameGraphPtr,
	RHISceneViewSnapshot&)
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
	if (debugPass)
	{
		debugPass->PreloadShader();
	}
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
	if (m_litPass && m_appliedParameterRevision == m_parameterRevision)
	{
		return;
	}

	if (!m_litPass)
	{
		m_litPass = TRefPtr<BlitNode>::Make();
	}
	CopyResource(*m_litPass, "src"_h, "src"_h);
	CopyResource(*m_litPass, "dst"_h, "dst"_h);

	constexpr std::array modes{
		ESceneViewRenderMode::AmbientOcclusion,
		ESceneViewRenderMode::Cascades,
		ESceneViewRenderMode::LightTiles
	};
	for (size_t index = 0u; index < modes.size(); ++index)
	{
		auto& pass = m_debugPasses[index];
		if (!pass)
		{
			pass = TRefPtr<PostProcessNode>::Make();
		}
		pass->SetString("shader"_h, GetString("shader"_h));
		pass->SetString(
			"defines"_h,
			GetSceneViewRenderModeShaderDefine(modes[index]));
		CopyResource(*pass, "color"_h, "dst"_h);
		CopyResource(*pass, "ldrSceneSampler"_h, "src"_h);
		CopyResource(*pass, "linearDepthSampler"_h, "linearDepth"_h);
	}
	m_appliedParameterRevision = m_parameterRevision;
}

void DebugViewNode::CopyResource(
	BaseFrameGraphNode& destination,
	StringHash destinationName,
	StringHash sourceName)
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
