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

void DebugDrawNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
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

	const auto depthAttachmentLayout = RHI::IsDepthStencilFormat(depthAttachment->GetFormat()) ? EImageLayout::DepthStencilAttachmentOptimal : EImageLayout::DepthAttachmentOptimal;

	commands->ImageMemoryBarrier(commandList, target, EImageLayout::ColorAttachmentOptimal);
	commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachmentLayout);

	{
		SAILOR_PROFILE_SCOPE("Wait for DebugContext");
		while (!sceneView.m_debugDrawSecondaryCmdList->IsFinished());
	}

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
			0.0f,
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
			0.0f,
			false);
	}

	commands->EndDebugRegion(commandList);
}

void DebugDrawNode::Clear()
{
}
