#include "BloomNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/RenderTarget.h"
#include "RHI/Texture.h"
#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/AssetRegistry.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* BloomNode::m_name = "Bloom";
#endif

void BloomNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdCompute);

	RHI::RHIRenderTargetPtr bloomRenderTarget = GetResolvedAttachment("bloom").DynamicCast<RHIRenderTarget>();

	const size_t numMipBindings = bloomRenderTarget->GetMipLevels() - 1;

	if (!m_pComputeDownscaleShader)
	{
		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeBloomDownscale.shader"))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetFileId(), m_pComputeDownscaleShader);
		}
	}

	if (!m_pComputeUpscaleShader)
	{
		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeBloomUpscale.shader"))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetFileId(), m_pComputeUpscaleShader);
		}
	}

	if (!m_pComputeUpscaleShader || !m_pComputeDownscaleShader ||
		!m_pComputeUpscaleShader->IsReady() || !m_pComputeDownscaleShader->IsReady())
	{
		return;
	}

	if (m_computeUpscaleBindings.Num() == 0)
	{
		RHI::RHITexturePtr lensDirtTexture = frameGraph->GetSampler("g_lensDirtSampler");

		m_computeUpscaleBindings.Resize(bloomRenderTarget->GetMipLevels());

		for (uint32_t i = (uint32_t)bloomRenderTarget->GetMipLevels() - 1; i >= 1; --i)
		{
			auto readMipLevel = bloomRenderTarget->GetMipLayer(i);
			auto writeMipLevel = bloomRenderTarget->GetMipLayer(i - 1);

			m_computeUpscaleBindings[i] = driver->CreateShaderBindings();
			driver->AddSamplerToShaderBindings(m_computeUpscaleBindings[i], "u_dirt_texture", lensDirtTexture, 2);

			driver->AddStorageImageToShaderBindings(m_computeUpscaleBindings[i], "u_input_texture", readMipLevel, 0);
			driver->AddStorageImageToShaderBindings(m_computeUpscaleBindings[i], "u_output_image", writeMipLevel, 1);
		}
	}

	if (m_computeDownscaleBindings.Num() == 0)
	{
		m_computeDownscaleBindings.Resize(numMipBindings);
	
		for (uint32_t i = 0; i < bloomRenderTarget->GetMipLevels() - 1; ++i)
		{
			auto readMipLevel = bloomRenderTarget->GetMipLayer(i);
			auto writeMipLevel = bloomRenderTarget->GetMipLayer(i + 1);

			m_computeDownscaleBindings[i] = driver->CreateShaderBindings();
			driver->AddStorageImageToShaderBindings(m_computeDownscaleBindings[i], "u_input_texture", readMipLevel, 0);
			driver->AddStorageImageToShaderBindings(m_computeDownscaleBindings[i], "u_output_image", writeMipLevel, 1);
		}
	}

	const glm::vec4 threshold = GetVec4("threshold");
	const glm::vec4 knee = GetVec4("knee");

	PushConstantsDownscale downscaleParams{};
	downscaleParams.m_threshold = glm::vec4(threshold.x, threshold.x - knee.x, 2.0f * knee.x, 0.25f * knee.x);

	commands->ImageMemoryBarrier(commandList, bloomRenderTarget, EImageLayout::General);

	// Bloom Downscale
	for (uint32_t i = 0; i < bloomRenderTarget->GetMipLevels() - 1; ++i)
	{
		downscaleParams.m_useThreshold = i == 0;

		auto readMipLevel = bloomRenderTarget->GetMipLayer(i);
		auto writeMipLevel = bloomRenderTarget->GetMipLayer(i + 1);

		const glm::uvec2 mipSize = glm::uvec2(writeMipLevel->GetExtent().x, writeMipLevel->GetExtent().y);

		commands->ImageMemoryBarrier(commandList, readMipLevel, EImageLayout::ComputeRead);
		commands->ImageMemoryBarrier(commandList, writeMipLevel,EImageLayout::ComputeWrite);

		commands->Dispatch(commandList, m_pComputeDownscaleShader->GetComputeShaderRHI(),
			(uint32_t)glm::ceil(float(mipSize.x) / 8),
			(uint32_t)glm::ceil(float(mipSize.y) / 8),
			1u,
			{ m_computeDownscaleBindings[i] },
			&downscaleParams, sizeof(PushConstantsDownscale));
	}

	PushConstantsUpscale upscaleParams{};
	upscaleParams.m_bloomIntensity = GetVec4("bloomIntensity").x;
	upscaleParams.m_dirtIntensity = GetVec4("dirtIntensity").x;

	// Bloom Upscale
	for (uint32_t i = (uint32_t)bloomRenderTarget->GetMipLevels() - 1; i >= 1; --i)
	{
		auto readMipLevel = bloomRenderTarget->GetMipLayer(i);
		auto writeMipLevel = bloomRenderTarget->GetMipLayer(i - 1);

		const glm::uvec2 mipSize = glm::uvec2(writeMipLevel->GetExtent().x, writeMipLevel->GetExtent().y);

		upscaleParams.m_mipLevel = i;

		commands->ImageMemoryBarrier(commandList, readMipLevel, EImageLayout::ComputeRead);
		commands->ImageMemoryBarrier(commandList, writeMipLevel, EImageLayout::ComputeWrite);

		commands->Dispatch(commandList, m_pComputeUpscaleShader->GetComputeShaderRHI(),
			(uint32_t)(glm::ceil(float(mipSize.x) / 8)),
			(uint32_t)(glm::ceil(float(mipSize.y) / 8)),
			1u,
			{ m_computeUpscaleBindings[i] },
			&upscaleParams, sizeof(PushConstantsUpscale));
	}

	commands->EndDebugRegion(commandList);
}

void BloomNode::Clear()
{
}
