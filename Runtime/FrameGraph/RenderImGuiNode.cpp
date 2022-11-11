#include "RenderImGuiNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/Texture.h"
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

void RenderImGuiNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	
	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	RHI::RHITexturePtr colorAttachment = GetResourceParam("color").DynamicCast<RHI::RHITexture>();
	if (!colorAttachment)
	{
		colorAttachment = frameGraph->GetRenderTarget("BackBuffer");
	}

	RHI::RHITexturePtr depthAttachment = GetResourceParam("depthStencil").DynamicCast<RHI::RHITexture>();
	if (!depthAttachment)
	{
		depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");
	}

	commands->RenderSecondaryCommandBuffers(commandList,
		{ sceneView.m_drawImGui },
		TVector<RHI::RHITexturePtr>{ colorAttachment },
		depthAttachment,
		glm::vec4(0, 0, colorAttachment->GetExtent().x, colorAttachment->GetExtent().y),
		glm::ivec2(0, 0),
		false,
		glm::vec4(0.0f),
		false);
}