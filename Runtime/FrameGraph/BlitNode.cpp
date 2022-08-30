#include "BlitNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
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

	RHI::RHITexturePtr src = GetResourceParam("src").StaticCast<RHITexture>();
	RHI::RHITexturePtr dst = GetResourceParam("dst").StaticCast<RHITexture>();
	if (!dst)
	{
		dst = frameGraph->GetRenderTarget("BackBuffer");
	}

	//glm::vec4 srcRegion = GetVectorParam("srcRegion");
	//glm::vec4 dstRegion = GetVectorParam("dstRegion");
	glm::ivec4 srcRegion(0, 0, src->GetExtent().x, src->GetExtent().y);
	glm::ivec4 dstRegion(0, 0, dst->GetExtent().x, dst->GetExtent().y);

	commands->BlitImage(commandList, src, dst, srcRegion, dstRegion);
}

void BlitNode::Clear()
{
}
