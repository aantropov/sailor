#include "RHIDebugDrawNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "Engine/World.h"
#include "Engine/GameObject.h"

using namespace Sailor;
using namespace Sailor::RHI;

#ifndef _SAILOR_IMPORT_
const char* RHIDebugDrawNode::m_name = "DebugDraw";
#endif

void RHIDebugDrawNode::Process(RHIFrameGraph* frameGraph, TVector<RHI::RHICommandListPtr>& transferCommandLists, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	auto colorAttachment = frameGraph->GetRenderTarget("BackBuffer");
	auto depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");

	auto& debugContext = sceneView.m_camera->GetOwner().StaticCast<GameObject>()->GetWorld()->GetDebugContext();

	commands->BeginRenderPass(commandList,
		TVector<RHI::RHITexturePtr>{ colorAttachment },
		depthAttachment,
		glm::vec4(0, 0, colorAttachment->GetExtent().x, colorAttachment->GetExtent().y),
		glm::ivec2(0, 0),
		false,
		glm::vec4(0.0f));
	debugContext->DrawDebugMesh(commandList, sceneView.m_frameBindings);
	commands->EndRenderPass(commandList);
}

void RHIDebugDrawNode::Clear()
{
}
