#include "RHIBlitNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "Engine/World.h"
#include "Engine/GameObject.h"

using namespace Sailor;
using namespace Sailor::RHI;

#ifndef _SAILOR_IMPORT_
const char* RHIBlitNode::m_name = "Blit";
#endif

void RHIBlitNode::Process(RHIFrameGraph* frameGraph, TVector<RHI::RHICommandListPtr>& transferCommandLists, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
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
	glm::ivec4 dstRegion(0, 0, src->GetExtent().x, src->GetExtent().y);

	commands->BlitImage(commandList, src, dst, srcRegion, dstRegion);
}

void RHIBlitNode::Clear()
{
}
