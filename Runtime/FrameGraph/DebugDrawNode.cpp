#include "DebugDrawNode.h"
#include "RHI/SceneView.h"
#include "RHI/CommandList.h"
#include "RHI/Renderer.h"
#include "RHI/Surface.h"
#include "RHI/RenderTarget.h"
#include "RHI/Texture.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* DebugDrawNode::m_name = "DebugDraw";
#endif

void DebugDrawNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	ResetDrawCallStats();

	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	auto colorAttachmentSurface = GetRHIResource("color", frameGraph.GetRawPtr()).DynamicCast<RHI::RHISurface>();
	auto target = GetTargetAttachment("color", frameGraph.GetRawPtr());

	auto depthAttachment = GetTargetAttachment("depthStencil", frameGraph.GetRawPtr());
	if (!depthAttachment)
	{
		const auto surface = frameGraph->GetSurface("DepthBuffer");
		depthAttachment = surface ? surface->GetTarget() : frameGraph->GetRenderTarget("DepthBuffer");
	}

	if (!target || !depthAttachment || !sceneView.m_debugDrawSecondaryCmdList)
	{
		return;
	}

	auto debugDrawCommandList = sceneView.m_debugDrawSecondaryCmdList->GetResult();
	if (!debugDrawCommandList)
	{
		return;
	}

	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdDebug);

	const auto depthAttachmentLayout = RHI::IsDepthStencilFormat(depthAttachment->GetFormat()) ? EImageLayout::DepthStencilAttachmentOptimal : EImageLayout::DepthAttachmentOptimal;

	commands->ImageMemoryBarrier(commandList, target, EImageLayout::ColorAttachmentOptimal);
	if (colorAttachmentSurface && colorAttachmentSurface->NeedsResolve())
	{
		commands->ImageMemoryBarrier(commandList, colorAttachmentSurface->GetResolved(), EImageLayout::ColorAttachmentOptimal);
	}
	commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachmentLayout);

	m_drawCallStats += debugDrawCommandList->GetRecordedDrawCallStats();

	if (colorAttachmentSurface)
	{
		commands->RenderSecondaryCommandBuffers(commandList,
			TVector<RHI::RHICommandListPtr> {debugDrawCommandList},
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
			TVector<RHI::RHICommandListPtr> {debugDrawCommandList},
			TVector<RHI::RHITexturePtr>{ target },
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
