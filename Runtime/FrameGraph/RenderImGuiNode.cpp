#include "RenderImGuiNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/Texture.h"
#include "RHI/RenderTarget.h"
#include "RHI/CommandList.h"
#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "Submodules/ImGuiApi.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* RenderImGuiNode::m_name = "RenderImGui";
#endif

void RenderImGuiNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	ResetDrawCallStats();

	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdDebug);

	RHI::RHITexturePtr colorAttachment = GetResolvedAttachment("color");
	for (const auto& r : m_unresolvedResourceParams)
	{
		if (r.First() == "color")
		{
			colorAttachment = frameGraph->GetRenderTarget(*r.Second());
			break;
		}
	}

	RHI::RHITexturePtr depthAttachment = GetResolvedAttachment("depthStencil");
	for (const auto& r : m_unresolvedResourceParams)
	{
		if (r.First() == "depthStencil")
		{
			depthAttachment = frameGraph->GetRenderTarget(*r.Second());
			break;
		}
	}

	if (!colorAttachment || !depthAttachment)
		return;

	{
		SAILOR_PROFILE_SCOPE("Wait for ImGui");
		while (!sceneView.m_drawImGui->IsFinished());
	}

	auto imguiCommandList = sceneView.m_drawImGui->GetResult();
	if (!imguiCommandList)
	{
		commands->EndDebugRegion(commandList);
		return;
	}

	m_drawCallStats += imguiCommandList->GetRecordedDrawCallStats();
	commands->RenderSecondaryCommandBuffers(commandList,
		{ imguiCommandList },
		TVector<RHI::RHITexturePtr>{ colorAttachment },
		depthAttachment,
		glm::vec4(0, 0, colorAttachment->GetExtent().x, colorAttachment->GetExtent().y),
		glm::ivec2(0, 0),
		false,
		glm::vec4(0.0f),
		0.0f,
		false);

	commands->EndDebugRegion(commandList);
}
