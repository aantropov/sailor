#include "DebugDrawNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/RenderTarget.h"
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

	auto colorAttachment = GetRHIResource("color").DynamicCast<RHI::RHISurface>();
	auto depthAttachment = GetRHIResource("depthStencil").DynamicCast<RHI::RHITexture>();
	if (!depthAttachment)
	{
		depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");
	}

	if (!colorAttachment)
	{
		return;
	}

	auto target = colorAttachment->GetTarget();

	commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), target->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);

	SAILOR_PROFILE_BLOCK("Wait for DebugContext");
	while (!sceneView.m_debugDrawSecondaryCmdList->IsFinished());
	SAILOR_PROFILE_END_BLOCK();

	commands->RenderSecondaryCommandBuffers(commandList,
		TVector<RHI::RHICommandListPtr> {sceneView.m_debugDrawSecondaryCmdList->GetResult()},
		TVector<RHI::RHISurfacePtr>{ colorAttachment },
		depthAttachment,
		glm::vec4(0, 0, target->GetExtent().x, target->GetExtent().y),
		glm::ivec2(0, 0),
		false,
		glm::vec4(0.0f),
		true);

	commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), EImageLayout::ColorAttachmentOptimal, target->GetDefaultLayout());
}

void DebugDrawNode::Clear()
{
}
