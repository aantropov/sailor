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
	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdDebug);

	auto colorAttachmentSurface = GetRHIResource("color").DynamicCast<RHI::RHISurface>();
	auto colorAttachmentRT = GetRHIResource("color").DynamicCast<RHI::RHIRenderTarget>();
	auto target = GetResolvedAttachment("color");

	auto depthAttachment = GetRHIResource("depthStencil").DynamicCast<RHI::RHITexture>();
	if (!depthAttachment)
	{
		depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");
	}

	if (!colorAttachmentSurface && !colorAttachmentRT)
	{
		return;
	}

	commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), target->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);
	commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), depthAttachment->GetDefaultLayout(), EImageLayout::DepthAttachmentOptimal);

	SAILOR_PROFILE_BLOCK("Wait for DebugContext");
	while (!sceneView.m_debugDrawSecondaryCmdList->IsFinished());
	SAILOR_PROFILE_END_BLOCK();

	if (colorAttachmentSurface)
	{
		commands->RenderSecondaryCommandBuffers(commandList,
			TVector<RHI::RHICommandListPtr> {sceneView.m_debugDrawSecondaryCmdList->GetResult()},
			TVector<RHI::RHISurfacePtr>{ colorAttachmentSurface },
			depthAttachment,
			glm::vec4(0, 0, target->GetExtent().x, target->GetExtent().y),
			glm::ivec2(0, 0),
			false,
			glm::vec4(0.0f),
			true);
	}
	else
	{
		commands->RenderSecondaryCommandBuffers(commandList,
			TVector<RHI::RHICommandListPtr> {sceneView.m_debugDrawSecondaryCmdList->GetResult()},
			TVector<RHI::RHITexturePtr>{ colorAttachmentRT },
			depthAttachment,
			glm::vec4(0, 0, target->GetExtent().x, target->GetExtent().y),
			glm::ivec2(0, 0),
			false,
			glm::vec4(0.0f),
			false);
	}
	commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), EImageLayout::ColorAttachmentOptimal, target->GetDefaultLayout());
	commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), EImageLayout::DepthAttachmentOptimal, depthAttachment->GetDefaultLayout());

	commands->EndDebugRegion(commandList);
}

void DebugDrawNode::Clear()
{
}
