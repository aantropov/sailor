#include "ClearNode.h"
#include "RHI/Renderer.h"
#include "RHI/Surface.h"
#include "RHI/RenderTarget.h"
#include "RHI/Texture.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* ClearNode::m_name = "Clear";
#endif

void ClearNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto renderer = App::GetSubmodule<RHI::Renderer>();
	auto commands = renderer->GetDriverCommands();

	auto resource = GetRHIResource("target", frameGraph.GetRawPtr());
	const auto surface = resource.DynamicCast<RHISurface>();
	const RHITexturePtr resolved = surface ? RHITexturePtr(surface->GetResolved()) : resource.DynamicCast<RHITexture>();
	if (!resolved)
	{
		SAILOR_LOG_ERROR("ClearNode '%s' cannot resolve its target render texture.", GetTag().c_str());
		return;
	}

	const auto clearTexture = [&](const RHITexturePtr& texture, const char* label = GetName())
	{
		commands->ImageMemoryBarrier(commandList, texture, EImageLayout::TransferDstOptimal);
		if (RHI::IsDepthFormat(texture->GetFormat()))
		{
			commands->BeginDebugRegion(commandList, label, glm::vec4(1.0f));
			commands->ClearDepthStencil(commandList, texture, GetFloat("clearDepth"), static_cast<uint32_t>(GetFloat("clearStencil")));
		}
		else
		{
			const glm::vec4 clearColor = GetVec4("clearColor");
			commands->BeginDebugRegion(commandList, label, glm::vec4(clearColor.x, clearColor.y, clearColor.z, 0.5f));
			commands->ClearImage(commandList, texture, clearColor);
		}
		commands->EndDebugRegion(commandList);
	};

	if (surface)
	{
		clearTexture(surface->GetTarget());
		if (!surface->NeedsResolve()) return;
	}

	// The driver keeps the multisampled depth attachment separately from its
	// resolved texture. Clear it for explicitly declared graph targets too;
	// otherwise previous frames keep rejecting moving geometry's fragments.
	if (resolved == frameGraph->GetRenderTarget("DepthBuffer") &&
		RHI::IsDepthFormat(resolved->GetFormat()) && renderer->GetMsaaSamples() != EMsaaSamples::Samples_1)
	{
		auto msaaDepth = renderer->GetDriver()->GetOrAddMsaaFramebufferRenderTarget(resolved->GetFormat(), resolved->GetExtent());
		if (!msaaDepth) return;
		clearTexture(msaaDepth, "Clear internal MSAA depth render target");
		const auto layout = RHI::IsDepthStencilFormat(resolved->GetFormat()) ?
			EImageLayout::DepthStencilAttachmentOptimal : EImageLayout::DepthAttachmentOptimal;
		commands->ImageMemoryBarrier(commandList, msaaDepth, layout);
	}

	clearTexture(resolved);
}

void ClearNode::Clear()
{
}
