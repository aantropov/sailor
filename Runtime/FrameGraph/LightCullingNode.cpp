#include "LightCullingNode.h"
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
const char* LightCullingNode::m_name = "LightCulling";
#endif

void LightCullingNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	if (!sceneView.m_rhiLightsData)
	{
		// No point to cull lights if we have no lights in the scene
		return;
	}

	if (!m_pComputeShader)
	{
		auto computeShaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeLightCulling.shader");
		App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(computeShaderInfo->GetUID(), m_pComputeShader);
	}

	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	auto depthAttachment = GetResourceParam("depthStencil").DynamicCast<RHI::RHITexture>();
	if (!depthAttachment)
	{
		depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");
	}

#ifdef _DEBUG
	if (RHIShaderPtr computeShader = m_pComputeShader->GetDebugComputeShaderRHI())
#else
	if (RHIShaderPtr computeShader = m_pComputeShader->GetComputeShaderRHI())
#endif
	{
		PushConstants pushConstants{};

		pushConstants.m_invViewProjection = sceneView.m_camera->GetInvViewProjection();
		pushConstants.m_lightsNum = sceneView.m_numLights;
		pushConstants.m_viewportSize = depthAttachment->GetExtent();
		pushConstants.m_numTiles.x = (depthAttachment->GetExtent().x - 1) / (int32_t)TileSize + 1;
		pushConstants.m_numTiles.y = (depthAttachment->GetExtent().y - 1) / (int32_t)TileSize + 1;

		if (!m_culledLights)
		{
			m_culledLights = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
			RHI::RHIShaderBindingPtr storageBinding = Sailor::RHI::Renderer::GetDriver()->AddSsboToShaderBindings(m_culledLights, "culledLights", sizeof(uint32_t) * LightsPerTile, pushConstants.m_numTiles.x * pushConstants.m_numTiles.y, 0, true);
			RHI::RHIShaderBindingPtr depthSampler = Sailor::RHI::Renderer::GetDriver()->AddSamplerToShaderBindings(m_culledLights, "sceneDepth", depthAttachment, 1);
			
			auto shaderBindingSet = sceneView.m_rhiLightsData;
			Sailor::RHI::Renderer::GetDriver()->AddShaderBinding(shaderBindingSet, storageBinding, "culledLights", 1);
		}

		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), depthAttachment->GetDefaultLayout(), RHI::EImageLayout::ShaderReadOnlyOptimal);
		commands->Dispatch(commandList, computeShader,
			pushConstants.m_numTiles.x, pushConstants.m_numTiles.y, 1,
			{ sceneView.m_rhiLightsData, m_culledLights, sceneView.m_frameBindings },
			&pushConstants, sizeof(PushConstants));
		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), RHI::EImageLayout::ShaderReadOnlyOptimal, depthAttachment->GetDefaultLayout());		
	}
}

void LightCullingNode::Clear()
{
	m_pComputeShader.Clear();
	m_culledLights.Clear();
}
