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

void ClearNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	glm::vec4 clearColor = GetVec4("clearColor");

	commands->BeginDebugRegion(commandList, GetName(), glm::vec4(clearColor.x, clearColor.y, clearColor.z, 0.5f));

	RHITexturePtr dst{};

	if (RHI::RHISurfacePtr surfaceAttachment = GetRHIResource("color").DynamicCast<RHISurface>())
	{
		RHITexturePtr dst2 = surfaceAttachment->GetTarget();

		commands->ImageMemoryBarrier(commandList, dst2, dst2->GetFormat(), dst2->GetDefaultLayout(), EImageLayout::TransferDstOptimal);
		commands->ClearImage(commandList, dst2, clearColor);
		commands->ImageMemoryBarrier(commandList, dst2, dst2->GetFormat(), EImageLayout::TransferDstOptimal, dst2->GetDefaultLayout());

		dst = surfaceAttachment->GetResolved();
	}
	else if (RHI::RHITexturePtr colorAttachment = GetRHIResource("color").DynamicCast<RHITexture>())
	{
		dst = colorAttachment;
	}
	else
	{
		auto backBuffer = frameGraph->GetRenderTarget("BackBuffer");
		dst = backBuffer;
	}

	commands->ImageMemoryBarrier(commandList, dst, dst->GetFormat(), dst->GetDefaultLayout(), EImageLayout::TransferDstOptimal);
	commands->ClearImage(commandList, dst, clearColor);
	commands->ImageMemoryBarrier(commandList, dst, dst->GetFormat(), EImageLayout::TransferDstOptimal, dst->GetDefaultLayout());

	commands->EndDebugRegion(commandList);
}

void ClearNode::Clear()
{
}
