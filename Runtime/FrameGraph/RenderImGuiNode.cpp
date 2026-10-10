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

void RenderImGuiNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	ResetDrawCallStats();

	RHI::RHITexturePtr colorAttachment = GetResolvedAttachment("color"_h, frameGraph.GetRawPtr());
	RHI::RHITexturePtr depthAttachment = GetResolvedAttachment("depthStencil"_h, frameGraph.GetRawPtr());

	if (!colorAttachment || !depthAttachment)
	{
		return;
	}

	{
		SAILOR_PROFILE_SCOPE("Wait for ImGui");
		sceneView.m_drawImGui->Wait();
	}

	auto imguiCommandList = sceneView.m_drawImGui->GetResult();
	if (!imguiCommandList)
	{
		return;
	}

	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdDebug);

	commands->ImageMemoryBarrier(commandList, colorAttachment, EImageLayout::ColorAttachmentOptimal);
	commands->ImageMemoryBarrier(commandList, depthAttachment, IsDepthStencilFormat(depthAttachment->GetFormat()) ?
		EImageLayout::DepthStencilAttachmentOptimal : EImageLayout::DepthAttachmentOptimal);

	if (commands->RenderSecondaryCommandBuffers(commandList,
		{ imguiCommandList },
		TVector<RHI::RHITexturePtr>{ colorAttachment },
		depthAttachment,
		glm::vec4(0, 0, colorAttachment->GetExtent().x, colorAttachment->GetExtent().y),
		glm::ivec2(0, 0),
		false,
		glm::vec4(0.0f),
		0.0f,
		false))
	{
		m_drawCallStats += imguiCommandList->GetRecordedDrawCallStats();
	}

	commands->EndDebugRegion(commandList);
}
