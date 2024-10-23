#include "ClearNode.h"
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
const char* ClearNode::m_name = "Clear";
#endif

void ClearNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto renderer = App::GetSubmodule<RHI::Renderer>();
	auto commands = renderer->GetDriverCommands();

	RHITexturePtr dst{};

	if (RHI::RHISurfacePtr surfaceAttachment = GetRHIResource("target").DynamicCast<RHISurface>())
	{
		RHITexturePtr dst2 = surfaceAttachment->GetTarget();

		commands->ImageMemoryBarrier(commandList, dst2, EImageLayout::TransferDstOptimal);
		if (RHI::IsDepthFormat(dst2->GetFormat()))
		{
			float clearDepth = GetFloat("clearDepth");
			float clearStencil = GetFloat("clearStencil");

			commands->BeginDebugRegion(commandList, GetName(), glm::vec4(1.0f));
			commands->ClearDepthStencil(commandList, dst2, clearDepth, (uint32_t)clearStencil);
			commands->EndDebugRegion(commandList);
		}
		else
		{
			glm::vec4 clearColor = GetVec4("clearColor");
			commands->BeginDebugRegion(commandList, GetName(), glm::vec4(clearColor.x, clearColor.y, clearColor.z, 0.5f));
			commands->ClearImage(commandList, dst2, clearColor);
			commands->EndDebugRegion(commandList);
		}

		dst = surfaceAttachment->GetResolved();

		if (!surfaceAttachment->NeedsResolve())
		{
			return;
		}
	}
	else if (RHI::RHITexturePtr colorAttachment = GetRHIResource("target").DynamicCast<RHITexture>())
	{
		dst = colorAttachment;
	}
	else
	{
		for (const auto& r : m_unresolvedResourceParams)
		{
			if (r.First() == "target")
			{
				dst = frameGraph->GetRenderTarget(*r.Second());

				// Hack: MSAA render targets are resolved inside the VulkanDriver
				// We need to clear internal msaa depth target
				if (*r.Second() == "DepthBuffer" && renderer->GetMsaaSamples() != EMsaaSamples::Samples_1)
				{
					auto msaaDepthTarget = renderer->GetDriver()->GetOrAddMsaaFramebufferRenderTarget(dst->GetFormat(), dst->GetExtent());
					check(msaaDepthTarget && msaaDepthTarget.IsValid() && RHI::IsDepthFormat(dst->GetFormat()));

					float clearDepth = GetFloat("clearDepth");
					float clearStencil = GetFloat("clearStencil");

					commands->ImageMemoryBarrier(commandList, msaaDepthTarget, RHI::EImageLayout::TransferDstOptimal);
					commands->BeginDebugRegion(commandList, "Clear internal MSAA depth render target", glm::vec4(1.0f));
					commands->ClearDepthStencil(commandList, msaaDepthTarget, clearDepth, (uint32_t)clearStencil);
					commands->EndDebugRegion(commandList);

					commands->ImageMemoryBarrier(commandList, msaaDepthTarget, RHI::EImageLayout::General);
				}

				break;
			}
		}
	}

	commands->ImageMemoryBarrier(commandList, dst, EImageLayout::TransferDstOptimal);
	if (RHI::IsDepthFormat(dst->GetFormat()))
	{
		float clearDepth = GetFloat("clearDepth");
		float clearStencil = GetFloat("clearStencil");

		commands->BeginDebugRegion(commandList, GetName(), glm::vec4(1.0f));
		commands->ClearDepthStencil(commandList, dst, clearDepth, (uint32_t)clearStencil);
		commands->EndDebugRegion(commandList);
	}
	else
	{
		glm::vec4 clearColor = GetVec4("clearColor");
		commands->BeginDebugRegion(commandList, GetName(), glm::vec4(clearColor.x, clearColor.y, clearColor.z, 0.5f));
		commands->ClearImage(commandList, dst, clearColor);
		commands->EndDebugRegion(commandList);
	}
}

void ClearNode::Clear()
{
}
