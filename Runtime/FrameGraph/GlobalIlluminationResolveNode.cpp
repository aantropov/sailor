#include "GlobalIlluminationResolveNode.h"

#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "RHI/CommandList.h"
#include "RHI/RenderTarget.h"
#include "RHI/Renderer.h"
#include "RHI/SceneView.h"
#include "RHI/Shader.h"

using namespace Sailor;
using namespace Sailor::Framegraph;
using namespace Sailor::RHI;

namespace
{
	constexpr uint32_t ResolveGroupSize = 8u;
}

void GlobalIlluminationResolveNode::Process(
	RHIFrameGraphPtr frameGraph,
	RHICommandListPtr,
	RHICommandListPtr commandList,
	const RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	ResetDrawCallStats();

	const bool bDebugProbes = sceneView.m_renderMode >= ESceneViewRenderMode::GlobalIlluminationProbes &&
		sceneView.m_renderMode <= ESceneViewRenderMode::GlobalIlluminationSubdivisions;
	if (!bDebugProbes && (!sceneView.m_bGlobalIlluminationEnabled ||
		sceneView.m_globalIlluminationMode == EGlobalIlluminationMode::NoGI))
	{
		return;
	}

	auto& driver = App::GetSubmodule<Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<Renderer>()->GetDriverCommands();

	RHITexturePtr depthTexture = GetSampledAttachment("depthSampler"_h, frameGraph.GetRawPtr());
	RHITexturePtr probeCellIndicesTexture =
		GetResolvedAttachment("probeCellIndices"_h, frameGraph.GetRawPtr());
	if (!depthTexture || !probeCellIndicesTexture)
	{
		return;
	}
	if (sceneView.m_globalIllumination && sceneView.m_globalIllumination->m_layout &&
		sceneView.m_globalIllumination->m_layout->m_bricks.Num() == 1u)
	{
		// A single grid has only one candidate. Material sampling still checks
		// its bounds and probe validity; no screen-space traversal is needed.
		commands->ImageMemoryBarrier(commandList, probeCellIndicesTexture, EImageLayout::TransferDstOptimal);
		commands->ClearImage(commandList, probeCellIndicesTexture, glm::vec4(1.0f));
		return;
	}
	if (!m_shader)
	{
		if (const auto shaderInfo = App::GetSubmodule<AssetRegistry>()
			->GetAssetInfoPtr("Shaders/GlobalIlluminationResolve.shader"))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader(
				shaderInfo->GetFileId(),
				m_shader);
		}
	}

	commands->BeginDebugRegion(
		commandList,
		GetName(),
		DebugContext::Color_CmdCompute);
	if (!m_shader || !m_shader->IsReady() || !sceneView.m_frameBindings ||
		!sceneView.m_rhiLightsData)
	{
		commands->ImageMemoryBarrier(
			commandList,
			probeCellIndicesTexture,
			EImageLayout::TransferDstOptimal);
		commands->ClearImage(
			commandList,
			probeCellIndicesTexture,
			glm::vec4(0.0f));
		commands->EndDebugRegion(commandList);
		return;
	}

	if (!m_bindings || m_depthTexture != depthTexture ||
		m_probeCellIndicesTexture != probeCellIndicesTexture)
	{
		m_bindings = driver->CreateShaderBindings();
		driver->AddSamplerToShaderBindings(
			m_bindings,
			"depthSampler"_h,
			depthTexture,
			0u);
		driver->AddStorageImageToShaderBindings(
			m_bindings,
			"probeCellIndices"_h,
			probeCellIndicesTexture,
			1u);
		m_bindings->RecalculateCompatibility();
		m_depthTexture = depthTexture;
		m_probeCellIndicesTexture = probeCellIndicesTexture;
	}

	commands->ImageMemoryBarrier(
		commandList,
		depthTexture,
		EImageLayout::ComputeRead);
	commands->ImageMemoryBarrier(
		commandList,
		probeCellIndicesTexture,
		EImageLayout::ComputeWrite);
	const glm::uvec2 extent(
		static_cast<uint32_t>(probeCellIndicesTexture->GetExtent().x),
		static_cast<uint32_t>(probeCellIndicesTexture->GetExtent().y));
	commands->Dispatch(
		commandList,
		m_shader->GetComputeShaderRHI(),
		(extent.x + ResolveGroupSize - 1u) / ResolveGroupSize,
		(extent.y + ResolveGroupSize - 1u) / ResolveGroupSize,
		1u,
		{ sceneView.m_frameBindings, sceneView.m_rhiLightsData, m_bindings });
	commands->EndDebugRegion(commandList);
}

void GlobalIlluminationResolveNode::Clear()
{
	m_shader.Clear();
	m_bindings.Clear();
	m_depthTexture.Clear();
	m_probeCellIndicesTexture.Clear();
}
