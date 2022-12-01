#include "EyeAdaptationNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/Texture.h"
#include "RHI/RenderTarget.h"
#include "RHI/Types.h"
#include "RHI/VertexDescription.h"
#include "Engine/World.h"
#include "Engine/GameObject.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* EyeAdaptationNode::m_name = "EyeAdaptation";
#endif

void EyeAdaptationNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	commands->BeginDebugRegion(commandList, GetName(), glm::vec4(1.0f, 0.65f, 0.0f, 0.25f));

	RHI::RHITexturePtr target = GetResolvedAttachment("color");
	RHI::RHITexturePtr depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");

	if (!m_pComputeHistogramShader)
	{
		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeHistogram.shader"))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetUID(), m_pComputeHistogramShader);
		}
	}

	if (!m_pComputeAverageShader)
	{
		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeAverageLuminance.shader"))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetUID(), m_pComputeAverageShader);
		}
	}

	if (!m_pToneMappingShader)
	{
		auto shaderPath = GetString("toneMappingShader");
		assert(!shaderPath.empty());

		auto definesStr = GetString("toneMappingDefines");
		TVector<std::string> defines = Sailor::Utils::SplitString(definesStr, " ");

		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(shaderPath))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetUID(), m_pToneMappingShader, defines);
		}
	}

	RHI::RHITexturePtr quarterResolution = GetResolvedAttachment("hdrColor");
	RHI::RHITexturePtr fullResolution = GetResolvedAttachment("colorSampler");

	if (!m_computeHistogramShaderBindings)
	{
		m_computeHistogramShaderBindings = driver->CreateShaderBindings();
		auto hitogramRes = driver->AddSsboToShaderBindings(m_computeHistogramShaderBindings, "histogram", sizeof(uint32_t), HistogramShades, 0, true);

		assert(quarterResolution);
		driver->AddStorageImageToShaderBindings(m_computeHistogramShaderBindings, "s_texColor", quarterResolution, 1);

		// We should init the buffer
		static TVector<uint32_t> initialData(HistogramShades);

		commands->UpdateShaderBinding(transferCommandList, hitogramRes,
			initialData.GetData(),
			sizeof(uint32_t) * HistogramShades,
			0);
	}

	if (!m_averageLuminance)
	{
		const ETextureUsageFlags usage = ETextureUsageBit::Storage_Bit | ETextureUsageBit::TextureTransferDst_Bit |
			ETextureUsageBit::Sampled_Bit;

		m_averageLuminance = driver->CreateRenderTarget(
			transferCommandList,
			glm::ivec2(1, 1),
			1,
			ETextureFormat::R16_SFLOAT,
			ETextureFiltration::Nearest,
			ETextureClamping::Repeat,
			usage);
	}

	if (!m_computeAverageShaderBindings)
	{
		auto& histogram = m_computeHistogramShaderBindings->GetOrAddShaderBinding("histogram");

		assert(histogram->IsBind());

		m_computeAverageShaderBindings = driver->CreateShaderBindings();
		driver->AddShaderBinding(m_computeAverageShaderBindings, histogram, "histogram", 0);
		driver->AddStorageImageToShaderBindings(m_computeAverageShaderBindings, "s_texColor", m_averageLuminance, 1);
	}

	if (!m_pToneMappingShader || !m_pToneMappingShader->IsReady() ||
		!m_pComputeHistogramShader || !m_pComputeHistogramShader->IsReady() ||
		!m_pComputeAverageShader || !m_pComputeAverageShader->IsReady() ||
		!target)
	{
		return;
	}

	if (!m_postEffectMaterial)
	{
		m_shaderBindings = driver->CreateShaderBindings();

		// Firstly we must assign the correct layout
		driver->FillShadersLayout(m_shaderBindings, { m_pToneMappingShader->GetDebugVertexShaderRHI(), m_pToneMappingShader->GetDebugFragmentShaderRHI() }, 1);

		// That should be enough to handle all the uniforms
		const size_t uniformsSize = m_vectorParams.Num() * sizeof(glm::vec4);
		if (uniformsSize > 0)
		{
			driver->AddBufferToShaderBindings(m_shaderBindings, "data", uniformsSize, 0, RHI::EShaderBindingType::UniformBuffer);
		}

		RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
		RenderState renderState{ false, false, 0, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, 0, false };
		m_postEffectMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pToneMappingShader, m_shaderBindings);

		for (auto& v : m_vectorParams)
		{
			commands->SetMaterialParameter(transferCommandList, m_shaderBindings, v.First(), v.Second());
		}

		if (m_vectorParams.ContainsKey("data.whitePoint"))
		{
			m_whitePointLum = glm::dot(glm::vec4(0.2125f, 0.7154f, 0.0721f, 0.0f), m_vectorParams["data.whitePoint"]);
		}

		driver->UpdateShaderBinding(m_shaderBindings, "colorSampler", fullResolution);
		driver->UpdateShaderBinding(m_shaderBindings, "averageLuminanceSampler", m_averageLuminance);
	}

	SAILOR_PROFILE_BLOCK("Image barriers");

	const float minLogLuminance = -8.0f;
	const float maxLogLuminance = 3.0f;
	const float eyeReaction = 1.8f;

	const float logLuminanceRange = maxLogLuminance - minLogLuminance;

	float pushConstantsHistogramm[] = { minLogLuminance, 1.0f / logLuminanceRange };

	float timeCoeff = std::clamp(1.0f - exp2(-sceneView.m_deltaTime * eyeReaction), 0.0f, 1.0f);

	float pushConstantsAverage[] =
	{
		minLogLuminance,
		logLuminanceRange,
		(float)quarterResolution->GetExtent().x * quarterResolution->GetExtent().y,
		timeCoeff
	};

	commands->ImageMemoryBarrier(commandList, quarterResolution, quarterResolution->GetFormat(), quarterResolution->GetDefaultLayout(), EImageLayout::ComputeRead);
	commands->Dispatch(commandList, m_pComputeHistogramShader->GetComputeShaderRHI(),
		quarterResolution->GetExtent().x / 16, quarterResolution->GetExtent().y / 16, 1,
		{ m_computeHistogramShaderBindings },
		&pushConstantsHistogramm, sizeof(float) * 2);
	commands->ImageMemoryBarrier(commandList, quarterResolution, quarterResolution->GetFormat(), EImageLayout::ComputeRead, quarterResolution->GetDefaultLayout());

	commands->ImageMemoryBarrier(commandList, m_averageLuminance, m_averageLuminance->GetFormat(), m_averageLuminance->GetDefaultLayout(), EImageLayout::ComputeWrite);
	commands->Dispatch(commandList, m_pComputeAverageShader->GetComputeShaderRHI(),
		1, 1, 1,
		{ m_computeAverageShaderBindings },
		&pushConstantsAverage, sizeof(float) * 4);
	commands->ImageMemoryBarrier(commandList, m_averageLuminance, m_averageLuminance->GetFormat(), EImageLayout::ComputeWrite, EImageLayout::ShaderReadOnlyOptimal);

	commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), depthAttachment->GetDefaultLayout(), EImageLayout::ShaderReadOnlyOptimal);
	commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), target->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);

	auto& fullResolutionBinding = m_shaderBindings->GetOrAddShaderBinding("colorSampler")->GetTextureBinding();
	commands->ImageMemoryBarrier(commandList, fullResolutionBinding, fullResolutionBinding->GetFormat(), fullResolutionBinding->GetDefaultLayout(), EImageLayout::ShaderReadOnlyOptimal);

	SAILOR_PROFILE_END_BLOCK();

	auto mesh = frameGraph->GetFullscreenNdcQuad();

	commands->BindMaterial(commandList, m_postEffectMaterial);
	commands->BindVertexBuffer(commandList, mesh->m_vertexBuffer, 0);
	commands->BindIndexBuffer(commandList, mesh->m_indexBuffer, 0);
	commands->BindShaderBindings(commandList, m_postEffectMaterial, { sceneView.m_frameBindings,  m_shaderBindings });

	commands->SetViewport(commandList,
		0, 0,
		(float)target->GetExtent().x, (float)target->GetExtent().y,
		glm::vec2(0, 0),
		glm::vec2(target->GetExtent().x, target->GetExtent().y),
		0, 1.0f);

	// TODO: Pipeline without depthAttachment
	commands->BeginRenderPass(commandList,
		TVector<RHI::RHITexturePtr>{target},
		depthAttachment,
		glm::vec4(0, 0, target->GetExtent().x, target->GetExtent().y),
		glm::ivec2(0, 0),
		false,
		glm::vec4(0.0f),
		false);

	const uint32_t firstIndex = (uint32_t)mesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
	const uint32_t vertexOffset = (uint32_t)mesh->m_vertexBuffer->GetOffset() / (uint32_t)mesh->m_vertexDescription->GetVertexStride();

	commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
	commands->EndRenderPass(commandList);

	SAILOR_PROFILE_BLOCK("Image barriers");

	commands->ImageMemoryBarrier(commandList, fullResolutionBinding, fullResolutionBinding->GetFormat(), EImageLayout::ShaderReadOnlyOptimal, fullResolutionBinding->GetDefaultLayout());
	commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), EImageLayout::ColorAttachmentOptimal, target->GetDefaultLayout());
	commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), EImageLayout::ShaderReadOnlyOptimal, depthAttachment->GetDefaultLayout());
	commands->ImageMemoryBarrier(commandList, m_averageLuminance, m_averageLuminance->GetFormat(), EImageLayout::ShaderReadOnlyOptimal, m_averageLuminance->GetDefaultLayout());

	SAILOR_PROFILE_END_BLOCK();

	commands->EndDebugRegion(commandList);
}

void EyeAdaptationNode::Clear()
{
	m_pToneMappingShader.Clear();
	m_postEffectMaterial.Clear();
	m_shaderBindings.Clear();
	m_pComputeHistogramShader.Clear();
	m_pComputeAverageShader.Clear();
}
