#include "LightCullingNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/Texture.h"
#include "RHI/RenderTarget.h"
#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "AssetRegistry/AssetRegistry.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* LightCullingNode::m_name = "LightCulling";
#endif

void LightCullingNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	if (!sceneView.m_rhiLightsData || !sceneView.m_rhiLightCullingData)
	{
		// No point to cull lights if we have no lights in the scene
		return;
	}

	if (!m_pComputeShader)
	{
		auto computeShaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeLightCulling.shader");
		App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(computeShaderInfo->GetFileId(), m_pComputeShader);
	}
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdCompute);

	auto linearDepthAttachment = GetRHIResource("linearDepth").DynamicCast<RHI::RHITexture>();
	if (!linearDepthAttachment)
	{
		linearDepthAttachment = frameGraph->GetRenderTarget("LinearDepth").DynamicCast<RHI::RHITexture>();
	}
	if (!linearDepthAttachment)
	{
		commands->EndDebugRegion(commandList);
		return;
	}

#ifdef _DEBUG
	if (RHIShaderPtr computeShader = m_pComputeShader->GetDebugComputeShaderRHI())
#else
	if (RHIShaderPtr computeShader = m_pComputeShader->GetComputeShaderRHI())
#endif
	{
		PushConstants pushConstants{};

		pushConstants.m_invViewProjection = sceneView.m_camera->GetInvViewProjection();
		pushConstants.m_lightsNum = sceneView.m_totalNumLights;
		pushConstants.m_viewportSize = linearDepthAttachment->GetExtent();
		pushConstants.m_numTiles.x = (linearDepthAttachment->GetExtent().x - 1) / (int32_t)TileSize + 1;
		pushConstants.m_numTiles.y = (linearDepthAttachment->GetExtent().y - 1) / (int32_t)TileSize + 1;
		commands->ImageMemoryBarrierForComputeSampling(commandList, linearDepthAttachment);
		commands->Dispatch(commandList, computeShader,
			pushConstants.m_numTiles.x, pushConstants.m_numTiles.y, 1,
			{ sceneView.m_rhiLightsData, sceneView.m_rhiLightCullingData, sceneView.m_frameBindings },
			&pushConstants, sizeof(PushConstants));

		commands->MemoryBarrier(commandList,
			static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit),
			static_cast<EAccessFlags>(EAccessBit::ShaderRead_Bit));
	}

	commands->EndDebugRegion(commandList);
}

void LightCullingNode::Clear()
{
	m_pComputeShader.Clear();
}
