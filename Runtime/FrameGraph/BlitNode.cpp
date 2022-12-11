#include "BlitNode.h"
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
const char* BlitNode::m_name = "Blit";
#endif

void BlitNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdTransfer);

	RHI::RHITexturePtr src = GetResolvedAttachment("src");
	RHI::RHITexturePtr dst = GetResolvedAttachment("dst");

	if (!dst)
	{
		dst = frameGraph->GetRenderTarget("BackBuffer");
	}

	//glm::vec4 srcRegion = GetVec4("srcRegion");
	//glm::vec4 dstRegion = GetVec4("dstRegion");
	glm::ivec4 srcRegion(0, 0, src->GetExtent().x, src->GetExtent().y);
	glm::ivec4 dstRegion(0, 0, dst->GetExtent().x, dst->GetExtent().y);

	commands->ImageMemoryBarrier(commandList, src, src->GetFormat(), src->GetDefaultLayout(), RHI::EImageLayout::TransferSrcOptimal);
	commands->ImageMemoryBarrier(commandList, dst, dst->GetFormat(), dst->GetDefaultLayout(), RHI::EImageLayout::TransferDstOptimal);

	commands->BlitImage(commandList, src, dst, srcRegion, dstRegion);

	if (RHISurfacePtr srcSurface = GetRHIResource("src").DynamicCast<RHISurface>())
	{
		if (RHISurfacePtr dstSurface = GetRHIResource("dst").DynamicCast<RHISurface>())
		{
			auto src2 = srcSurface->GetTarget();
			auto dst2 = dstSurface->GetTarget();

			if (srcSurface->NeedsResolve() && dstSurface->NeedsResolve())
			{
				commands->ImageMemoryBarrier(commandList, src2, src2->GetFormat(), src2->GetDefaultLayout(), RHI::EImageLayout::TransferSrcOptimal);
				commands->ImageMemoryBarrier(commandList, dst2, dst2->GetFormat(), dst2->GetDefaultLayout(), RHI::EImageLayout::TransferDstOptimal);

				commands->BlitImage(commandList, src2, dst2, srcRegion, dstRegion);

				commands->ImageMemoryBarrier(commandList, src2, src2->GetFormat(), RHI::EImageLayout::TransferSrcOptimal, src2->GetDefaultLayout());
				commands->ImageMemoryBarrier(commandList, dst2, dst2->GetFormat(), RHI::EImageLayout::TransferDstOptimal, dst2->GetDefaultLayout());
			}
		}
	}

	commands->ImageMemoryBarrier(commandList, src, src->GetFormat(), RHI::EImageLayout::TransferSrcOptimal, src->GetDefaultLayout());
	commands->ImageMemoryBarrier(commandList, dst, dst->GetFormat(), RHI::EImageLayout::TransferDstOptimal, dst->GetDefaultLayout());

	commands->EndDebugRegion(commandList);
}

void BlitNode::Clear()
{
}
