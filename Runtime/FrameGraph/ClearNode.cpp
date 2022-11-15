#include "ClearNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
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

	glm::vec4 clearColor = GetVec4("clearColor");

	if (RHI::RHISurfacePtr surfaceAttachment = GetRHIResource("color").DynamicCast<RHISurface>())
	{
		commands->ClearImage(commandList, surfaceAttachment->GetTarget(), clearColor);
		commands->ClearImage(commandList, surfaceAttachment->GetResolved(), clearColor);
	}
	else if (RHI::RHITexturePtr colorAttachment = GetRHIResource("color").DynamicCast<RHITexture>())
	{
		commands->ClearImage(commandList, colorAttachment, clearColor);
	}
	else
	{
		auto backBuffer = frameGraph->GetRenderTarget("BackBuffer");
		commands->ClearImage(commandList, backBuffer, clearColor);
	}
}

void ClearNode::Clear()
{
}
