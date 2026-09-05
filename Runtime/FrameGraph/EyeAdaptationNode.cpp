#include "EyeAdaptationNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "RHI/Types.h"
#include "AssetRegistry/AssetRegistry.h"

#include <algorithm>
#include <cmath>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* EyeAdaptationNode::m_name = "EyeAdaptation";
#endif

void EyeAdaptationNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	ResetDrawCallStats();

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	if (!m_pComputeHistogramShader)
	{
		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeHistogram.shader"))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetFileId(), m_pComputeHistogramShader);
		}
	}

	if (!m_pComputeAverageShader)
	{
		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeAverageLuminance.shader"))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetFileId(), m_pComputeAverageShader);
		}
	}

	RHI::RHITexturePtr hdrColor = GetResolvedAttachment("hdrColor");
	RHI::RHITexturePtr averageLuminance = GetResolvedAttachment("averageLuminance");
	if (!m_pComputeHistogramShader || !m_pComputeHistogramShader->IsReady() ||
		!m_pComputeAverageShader || !m_pComputeAverageShader->IsReady() ||
		!hdrColor || !averageLuminance)
	{
		return;
	}

	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdCompute);

	if (m_averageLuminanceTarget != averageLuminance)
	{
		m_averageLuminanceTarget = averageLuminance;
		m_bAverageLuminanceInitialized = false;
	}

	if (!m_bAverageLuminanceInitialized)
	{
		commands->ImageMemoryBarrier(commandList, averageLuminance, EImageLayout::TransferDstOptimal);
		commands->ClearImage(commandList, averageLuminance, glm::vec4(-1.0f));
		m_bAverageLuminanceInitialized = true;
	}

	if (!m_computeHistogramShaderBindings)
	{
		m_computeHistogramShaderBindings = driver->CreateShaderBindings();
		auto histogramResource = driver->AddSsboToShaderBindings(
			m_computeHistogramShaderBindings,
			"histogram",
			sizeof(uint32_t),
			HistogramShades,
			0,
			true);

		driver->AddStorageImageToShaderBindings(m_computeHistogramShaderBindings, "s_texColor", hdrColor, 1);
		static TVector<uint32_t> initialData(HistogramShades);

		commands->UpdateShaderBinding(transferCommandList, histogramResource,
			initialData.GetData(),
			sizeof(uint32_t) * HistogramShades,
			0);
	}
	else
	{
		driver->UpdateShaderBinding(
			m_computeHistogramShaderBindings,
			"s_texColor",
			hdrColor);
	}

	if (!m_computeAverageShaderBindings)
	{
		auto& histogram = m_computeHistogramShaderBindings->GetOrAddShaderBinding("histogram");

		check(histogram->IsBind());

		m_computeAverageShaderBindings = driver->CreateShaderBindings();
		driver->AddShaderBinding(m_computeAverageShaderBindings, histogram, "histogram", 0);
		driver->AddStorageImageToShaderBindings(m_computeAverageShaderBindings, "s_texColor", averageLuminance, 1);
	}
	else
	{
		driver->UpdateShaderBinding(
			m_computeAverageShaderBindings,
			"s_texColor",
			averageLuminance);
	}

	{
		SAILOR_PROFILE_SCOPE("Image barriers");

		const float minLogLuminance = -16.0f;
		const float maxLogLuminance = 16.0f;
		const glm::vec4 metering = GetVec4("metering");
		const glm::vec4 adaptation = GetVec4("adaptation");
		const float centerWeight = std::max(metering.x, 0.0f);
		const float lowPercentile = std::clamp(metering.y, 0.0f, 0.99f);
		const float highPercentile = std::clamp(
			metering.z,
			lowPercentile + 0.01f,
			1.0f);
		const float minEV100 = std::min(adaptation.x, adaptation.y);
		const float maxEV100 = std::max(adaptation.x, adaptation.y);
		const float minimumMeteredLuminance =
			std::exp2(minEV100) / 8.0f;
		const float speedUp = std::max(adaptation.z, 0.0f);
		const float speedDown = std::max(adaptation.w, 0.0f);

		const float logLuminanceRange = maxLogLuminance - minLogLuminance;

		const float pushConstantsHistogram[] =
		{
			minLogLuminance,
			1.0f / logLuminanceRange,
			centerWeight,
			minimumMeteredLuminance
		};

		const float pushConstantsAverage[] =
		{
			minLogLuminance,
			logLuminanceRange,
			lowPercentile,
			highPercentile,
			minEV100,
			maxEV100,
			std::max(sceneView.m_deltaTime, 0.0f),
			speedUp,
			speedDown,
			0.0f,
			0.0f,
			0.0f
		};

		commands->ImageMemoryBarrier(commandList, hdrColor, EImageLayout::ComputeRead);
		commands->Dispatch(commandList, m_pComputeHistogramShader->GetComputeShaderRHI(),
			(hdrColor->GetExtent().x + 15) / 16,
			(hdrColor->GetExtent().y + 15) / 16,
			1,
			{ m_computeHistogramShaderBindings },
			&pushConstantsHistogram, sizeof(pushConstantsHistogram));
		const EAccessFlags shaderReadWrite =
			static_cast<EAccessFlags>(EAccessBit::ShaderRead_Bit) |
			static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit);
		commands->MemoryBarrier(commandList,
			static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit),
			shaderReadWrite);

		commands->ImageMemoryBarrier(commandList, averageLuminance, EImageLayout::ComputeWrite);
		commands->Dispatch(commandList, m_pComputeAverageShader->GetComputeShaderRHI(),
			1, 1, 1,
			{ m_computeAverageShaderBindings },
			&pushConstantsAverage, sizeof(pushConstantsAverage));
		commands->ImageMemoryBarrier(commandList, averageLuminance, EImageLayout::ShaderReadOnlyOptimal);
	}

	commands->EndDebugRegion(commandList);
}

void EyeAdaptationNode::Clear()
{
	m_pComputeHistogramShader.Clear();
	m_pComputeAverageShader.Clear();
	m_computeHistogramShaderBindings.Clear();
	m_computeAverageShaderBindings.Clear();
	m_averageLuminanceTarget.Clear();
	m_bAverageLuminanceInitialized = false;
}
