#include "LightCullingNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "RHI/RenderTarget.h"
#include "RHI/Surface.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "AssetRegistry/AssetRegistry.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

void LightCullingNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	if (!sceneView.m_rhiLightsData)
	{
		return;
	}

	auto linearDepthAttachment = GetResolvedAttachment("linearDepth"_h, frameGraph.GetRawPtr());
	if (!linearDepthAttachment)
	{
		const auto surface = frameGraph->GetSurface("LinearDepth"_h);
		linearDepthAttachment = surface ? surface->GetResolved() : frameGraph->GetRenderTarget("LinearDepth"_h);
	}
	if (!linearDepthAttachment)
	{
		return;
	}

	auto resources = sceneView.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(
		this, sceneView.m_cameraIndex, 0u);
	auto& driver = Renderer::GetDriver();
	const auto extent = linearDepthAttachment->GetExtent();
	const size_t numTiles = static_cast<size_t>((extent.x - 1) / TileSize + 1) *
		((extent.y - 1) / TileSize + 1);
	if (resources->m_tileCapacity < numTiles)
	{
		resources->m_bindings = driver->CreateShaderBindings();
		driver->AddSsboToShaderBindings(resources->m_bindings, "culledLights"_h,
			sizeof(uint32_t) * numTiles * LightsPerTile, 1u, 0u, true);
		driver->AddSsboToShaderBindings(resources->m_bindings, "lightsGrid"_h,
			sizeof(uint32_t) * numTiles * 2u, 1u, 1u, true);
		resources->m_tileCapacity = numTiles;
		resources->m_publishedIndices.Clear();
	}
	auto& bindings = resources->m_bindings;
	auto lightsBindings = sceneView.m_rhiLightsData;
	if (!resources->m_publishedIndices ||
		lightsBindings->GetOrAddShaderBinding("culledLights"_h) != resources->m_publishedIndices)
	{
		// Rebind after tile growth, a new lighting set, or another culling node.
		resources->m_publishedIndices = driver->AddShaderBinding(lightsBindings,
			bindings->GetOrAddShaderBinding("culledLights"_h), "culledLights"_h, 1u);
		driver->AddShaderBinding(lightsBindings,
			bindings->GetOrAddShaderBinding("lightsGrid"_h), "lightsGrid"_h, 2u);
	}
	if (bindings->GetOrAddShaderBinding("linearDepth"_h)->GetTextureBinding() != linearDepthAttachment)
	{
		driver->AddSamplerToShaderBindings(bindings, "linearDepth"_h, linearDepthAttachment, 2u);
	}

	if (!m_pComputeShader)
	{
		auto computeShaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeLightCulling.shader");
		App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(computeShaderInfo->GetFileId(), m_pComputeShader);
	}
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdCompute);

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
			{ lightsBindings, bindings, sceneView.m_frameBindings },
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
