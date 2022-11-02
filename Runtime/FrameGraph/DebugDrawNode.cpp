#include "DebugDrawNode.h"
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
const char* DebugDrawNode::m_name = "DebugDraw";
#endif

void DebugDrawNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	auto colorAttachment = GetResourceParam("color").DynamicCast<RHI::RHISurface>();
	auto depthAttachment = GetResourceParam("depthStencil").DynamicCast<RHI::RHITexture>();
	if (!depthAttachment)
	{
		depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");
	}

	if (!colorAttachment)
	{
		return;
	}

	auto target = colorAttachment->GetResolved();

	commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), target->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);

	commands->RenderSecondaryCommandBuffers(commandList,
		TVector<RHI::RHICommandListPtr> {sceneView.m_debugDrawSecondaryCmdList},
		TVector<RHI::RHITexturePtr>{ target },
		depthAttachment,
		glm::vec4(0, 0, target->GetExtent().x, target->GetExtent().y),
		glm::ivec2(0, 0),
		false,
		glm::vec4(0.0f),
		false);

	commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), EImageLayout::ColorAttachmentOptimal, target->GetDefaultLayout());
}

void DebugDrawNode::Clear()
{
}
