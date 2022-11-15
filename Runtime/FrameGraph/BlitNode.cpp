#include "BlitNode.h"
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
const char* BlitNode::m_name = "Blit";
#endif

void BlitNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	RHI::RHITexturePtr src;
	if (RHISurfacePtr surface = GetRHIResource("src").DynamicCast<RHISurface>())
	{
		src = surface->GetResolved();
	}
	else
	{
		src = GetRHIResource("src").DynamicCast<RHITexture>();
	}

	RHI::RHITexturePtr dst = GetRHIResource("dst").DynamicCast<RHITexture>();
	if (!dst)
	{
		dst = frameGraph->GetRenderTarget("BackBuffer");
	}

	//glm::vec4 srcRegion = GetVec4("srcRegion");
	//glm::vec4 dstRegion = GetVec4("dstRegion");
	glm::ivec4 srcRegion(0, 0, src->GetExtent().x, src->GetExtent().y);
	glm::ivec4 dstRegion(0, 0, dst->GetExtent().x, dst->GetExtent().y);

	commands->BlitImage(commandList, src, dst, srcRegion, dstRegion);
}

void BlitNode::Clear()
{
}
