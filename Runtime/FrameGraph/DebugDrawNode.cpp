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

void DebugDrawNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	ResetDrawCallStats();

	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	auto colorAttachmentSurface = GetRHIResource("color"_h, frameGraph.GetRawPtr()).DynamicCast<RHI::RHISurface>();
	auto target = GetTargetAttachment("color"_h, frameGraph.GetRawPtr());

	auto depthResource = GetRHIResource("depthStencil"_h, frameGraph.GetRawPtr());
	if (!depthResource) depthResource = frameGraph->GetResource("DepthBuffer"_h);
	const auto depthSurface = depthResource.DynamicCast<RHISurface>();
	const bool bUsesMsaaDepth = colorAttachmentSurface && colorAttachmentSurface->NeedsResolve();
	const RHITexturePtr depthAttachment = depthSurface ?
		(bUsesMsaaDepth ? depthSurface->GetTarget() : depthSurface->GetResolved()) : depthResource.DynamicCast<RHITexture>();

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

	bool rendered;
	if (colorAttachmentSurface)
	{
		rendered = commands->RenderSecondaryCommandBuffers(commandList,
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
		rendered = commands->RenderSecondaryCommandBuffers(commandList,
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
	if (rendered) m_drawCallStats += debugDrawCommandList->GetRecordedDrawCallStats();

	commands->EndDebugRegion(commandList);
}

void DebugDrawNode::Clear()
{
}
