#include "RHIRenderSceneNode.h"
#include "RHI/SceneView.h"

using namespace Sailor;
using namespace Sailor::RHI;

#ifndef _SAILOR_IMPORT_
const char* RHIRenderSceneNode::m_name = "RenderScene";
#endif

void RHIRenderSceneNode::Initialize(RHIFrameGraphPtr FrameGraph)
{
}

void RHIRenderSceneNode::Process(const RHI::RHISceneViewSnapshot& sceneView)
{
}

void RHIRenderSceneNode::Clear()
{
}