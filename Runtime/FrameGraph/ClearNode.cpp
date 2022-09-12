#include "ClearNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "Engine/World.h"
#include "Engine/GameObject.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* ClearNode::m_name = "Clear";
#endif

void ClearNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	RHI::RHITexturePtr colorAttachment = GetResourceParam("color").StaticCast<RHITexture>();
	if (!colorAttachment)
	{
		colorAttachment = frameGraph->GetRenderTarget("BackBuffer");
	}

	glm::vec4 clearColor = GetVectorParam("clearColor");

	commands->ClearImage(commandList, colorAttachment, clearColor);
}

void ClearNode::Clear()
{
}
