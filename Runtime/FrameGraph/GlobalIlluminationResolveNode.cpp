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

#ifndef _SAILOR_IMPORT_
const char* GlobalIlluminationResolveNode::m_name =
	"GlobalIlluminationResolve";
#endif

namespace
{
	constexpr uint32_t ResolveGroupSize = 8u;
}

void GlobalIlluminationResolveNode::Process(
	RHIFrameGraphPtr,
	RHICommandListPtr,
	RHICommandListPtr commandList,
	const RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	ResetDrawCallStats();

	auto& driver = App::GetSubmodule<Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<Renderer>()->GetDriverCommands();

	RHITexturePtr depthTexture = GetResolvedAttachment("depthSampler");
	RHITexturePtr probeCellMinTexture =
		GetResolvedAttachment("probeCellMin");
	RHITexturePtr probeCellMaxTexture =
		GetResolvedAttachment("probeCellMax");
	RHITexturePtr probeCellMetadataTexture =
		GetResolvedAttachment("probeCellMetadata");
	if (!depthTexture || !probeCellMinTexture || !probeCellMaxTexture ||
		!probeCellMetadataTexture)
	{
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
		for (const RHITexturePtr& texture :
			{ probeCellMinTexture, probeCellMaxTexture, probeCellMetadataTexture })
		{
			commands->ImageMemoryBarrier(
				commandList,
				texture,
				EImageLayout::TransferDstOptimal);
			commands->ClearImage(
				commandList,
				texture,
				glm::vec4(0.0f));
		}
		commands->EndDebugRegion(commandList);
		return;
	}

	if (!m_bindings || m_depthTexture != depthTexture ||
		m_probeCellMinTexture != probeCellMinTexture ||
		m_probeCellMaxTexture != probeCellMaxTexture ||
		m_probeCellMetadataTexture != probeCellMetadataTexture)
	{
		m_bindings = driver->CreateShaderBindings();
		driver->AddSamplerToShaderBindings(
			m_bindings,
			"depthSampler",
			depthTexture,
			0u);
		driver->AddStorageImageToShaderBindings(
			m_bindings,
			"probeCellMin",
			probeCellMinTexture,
			1u);
		driver->AddStorageImageToShaderBindings(
			m_bindings,
			"probeCellMax",
			probeCellMaxTexture,
			2u);
		driver->AddStorageImageToShaderBindings(
			m_bindings,
			"probeCellMetadata",
			probeCellMetadataTexture,
			3u);
		m_bindings->RecalculateCompatibility();
		m_depthTexture = depthTexture;
		m_probeCellMinTexture = probeCellMinTexture;
		m_probeCellMaxTexture = probeCellMaxTexture;
		m_probeCellMetadataTexture = probeCellMetadataTexture;
	}

	commands->ImageMemoryBarrier(
		commandList,
		depthTexture,
		EImageLayout::ComputeRead);
	for (const RHITexturePtr& texture :
		{ probeCellMinTexture, probeCellMaxTexture, probeCellMetadataTexture })
	{
		commands->ImageMemoryBarrier(
			commandList,
			texture,
			EImageLayout::ComputeWrite);
	}
	const glm::uvec2 extent(
		static_cast<uint32_t>(probeCellMinTexture->GetExtent().x),
		static_cast<uint32_t>(probeCellMinTexture->GetExtent().y));
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
	m_probeCellMinTexture.Clear();
	m_probeCellMaxTexture.Clear();
	m_probeCellMetadataTexture.Clear();
}
