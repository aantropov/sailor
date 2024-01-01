#include "DepthHighZNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/RenderTarget.h"
#include "RHI/Texture.h"
#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "AssetRegistry/Texture/TextureImporter.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* DepthHighZNode::m_name = "DepthHighZ";
#endif

void DepthHighZNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	RHI::RHITexturePtr depthAttachment = GetRHIResource("src").DynamicCast<RHI::RHIRenderTarget>()->GetDepthAspect();
	if (!depthAttachment)
	{
		depthAttachment = frameGraph->GetRenderTarget("DepthBuffer")->GetDepthAspect();
	}

	RHI::RHIRenderTargetPtr highZRenderTarget = GetResolvedAttachment("dst").DynamicCast<RHIRenderTarget>();

	const size_t numMipBindings = highZRenderTarget->GetMipLevels() - 1;

	if (!m_pComputeDepthHighZShader)
	{
		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeDepthHighZ.shader"))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetFileId(), m_pComputeDepthHighZShader);
		}
	}

	if (!m_pComputeDepthHighZShader || !m_pComputeDepthHighZShader->IsReady())
	{
		return;
	}

	if (m_computeDepthHighZBindings.Num() == 0)
	{
		m_computeDepthHighZBindings.Resize(numMipBindings);

		for (uint32_t i = 0; i < highZRenderTarget->GetMipLevels() - 1; ++i)
		{
			auto readMipLevel = highZRenderTarget->GetMipLayer(i);
			auto writeMipLevel = highZRenderTarget->GetMipLayer(i + 1);

			m_computeDepthHighZBindings[i] = driver->CreateShaderBindings();
			driver->AddSamplerToShaderBindings(m_computeDepthHighZBindings[i], "inputDepth", readMipLevel, 0);
			driver->AddStorageImageToShaderBindings(m_computeDepthHighZBindings[i], "outputDepth", writeMipLevel, 1);
		}

		m_computePrepassDepthHighZBindings = driver->CreateShaderBindings();
		driver->AddSamplerToShaderBindings(m_computePrepassDepthHighZBindings, "inputDepth", depthAttachment, 0);
		driver->AddStorageImageToShaderBindings(m_computePrepassDepthHighZBindings, "outputDepth", highZRenderTarget->GetMipLayer(0), 1);
	}

	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdCompute);
	{
		commands->ImageMemoryBarrier(commandList, highZRenderTarget, highZRenderTarget->GetFormat(), highZRenderTarget->GetDefaultLayout(), EImageLayout::General);

		//Depth Downscale
		for (int32_t i = -1; i < (int32_t)highZRenderTarget->GetMipLevels() - 1; ++i)
		{
			const bool bFirst = i == -1;

			auto readMipLevel = bFirst ? depthAttachment : highZRenderTarget->GetMipLayer(i);
			auto writeMipLevel = highZRenderTarget->GetMipLayer(i + 1);

			const glm::uvec2 mipSize = glm::uvec2(writeMipLevel->GetExtent().x, writeMipLevel->GetExtent().y);

			PushConstantsDownscale params{};
			params.m_outputSize = glm::vec2(mipSize.x, mipSize.y);

			commands->ImageMemoryBarrier(commandList, readMipLevel, readMipLevel->GetFormat(), readMipLevel->GetDefaultLayout(), EImageLayout::ComputeRead);
			commands->ImageMemoryBarrier(commandList, writeMipLevel, writeMipLevel->GetFormat(), writeMipLevel->GetDefaultLayout(), EImageLayout::ComputeWrite);

			commands->Dispatch(commandList, m_pComputeDepthHighZShader->GetComputeShaderRHI(),
				(uint32_t)glm::ceil(float(mipSize.x) / 8),
				(uint32_t)glm::ceil(float(mipSize.y) / 8),
				1u,
				{ bFirst ? m_computePrepassDepthHighZBindings : m_computeDepthHighZBindings[i] },
				&params, sizeof(PushConstantsDownscale));

			commands->ImageMemoryBarrier(commandList, readMipLevel, readMipLevel->GetFormat(), EImageLayout::ComputeRead, readMipLevel->GetDefaultLayout());
			commands->ImageMemoryBarrier(commandList, writeMipLevel, writeMipLevel->GetFormat(), EImageLayout::ComputeWrite, writeMipLevel->GetDefaultLayout());
		}

		commands->ImageMemoryBarrier(commandList, highZRenderTarget, highZRenderTarget->GetFormat(), EImageLayout::General, highZRenderTarget->GetDefaultLayout());
	}
	commands->EndDebugRegion(commandList);
}

void DepthHighZNode::Clear()
{
}
