#include "DepthHighZNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/RenderTarget.h"
#include "RHI/Surface.h"
#include "RHI/Texture.h"
#include "AssetRegistry/AssetRegistry.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

void DepthHighZNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	auto depthResource = GetRHIResource("src"_h, frameGraph.GetRawPtr());
	if (!depthResource) depthResource = frameGraph->GetResource("DepthBuffer"_h);
	const auto depthSurface = depthResource.DynamicCast<RHISurface>();
	auto depthAttachment = depthSurface ? depthSurface->GetTarget() : depthResource.DynamicCast<RHIRenderTarget>();

	RHI::RHIRenderTargetPtr highZRenderTarget = GetResolvedAttachment("dst"_h, frameGraph.GetRawPtr()).DynamicCast<RHIRenderTarget>();
	if (!depthAttachment || !highZRenderTarget || highZRenderTarget->GetMipLevels() == 0u)
	{
		return;
	}

	const size_t numMipBindings = highZRenderTarget->GetMipLevels() - 1;
	if (!depthSurface && depthAttachment == frameGraph->GetRenderTarget("DepthBuffer"_h) &&
		App::GetSubmodule<RHI::Renderer>()->GetMsaaSamples() != EMsaaSamples::Samples_1)
	{
		// Read the attachment written by the depth prepasses, before native resolve
		// can lose uncovered samples. The device may clamp the requested MSAA count.
		auto msaaDepth = driver->GetOrAddMsaaFramebufferRenderTarget(
			depthAttachment->GetFormat(), depthAttachment->GetExtent());
		if (!msaaDepth) return;
		if (msaaDepth->GetMsaaSamples() != EMsaaSamples::Samples_1)
		{
			depthAttachment = msaaDepth.StaticCast<RHIRenderTarget>();
		}
	}
	const bool bMsaaDepth = depthAttachment->GetMsaaSamples() != EMsaaSamples::Samples_1;
	auto& inputShader = bMsaaDepth ? m_pComputeDepthHighZMsaaShader : m_pComputeDepthHighZInputShader;

	if (!m_pComputeDepthHighZShader || !inputShader)
	{
		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeDepthHighZ.shader"))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetFileId(), m_pComputeDepthHighZShader);
			App::GetSubmodule<ShaderCompiler>()->LoadShader(
				shaderInfo->GetFileId(), inputShader, { bMsaaDepth ? "MSAA_DEPTH_INPUT" : "DEPTH_INPUT" });
		}
	}

	if (!m_pComputeDepthHighZShader || !m_pComputeDepthHighZShader->IsReady() ||
		!inputShader || !inputShader->IsReady())
	{
		return;
	}

	if (m_boundDepth != depthAttachment || m_boundPyramid != highZRenderTarget ||
		m_computeDepthHighZBindings.Num() != numMipBindings)
	{
		m_computeDepthHighZBindings.Clear();
		m_computeDepthHighZBindings.Resize(numMipBindings);

		// Keep every descriptor valid at the tail; the pushed mip count prevents
		// writes through unused outputs that alias the final mip view.
		for (uint32_t i = 0; i < highZRenderTarget->GetMipLevels() - 1; ++i)
		{
			auto readMipLevel = highZRenderTarget->GetMipLayer(i);
			auto writeMipLevel = highZRenderTarget->GetMipLayer(i + 1);

			m_computeDepthHighZBindings[i] = driver->CreateShaderBindings();
			driver->AddStorageImageToShaderBindings(m_computeDepthHighZBindings[i], "inputDepth"_h, readMipLevel, 0);
			driver->AddStorageImageToShaderBindings(m_computeDepthHighZBindings[i], "outputDepth"_h, writeMipLevel, 1);
			driver->AddStorageImageToShaderBindings(m_computeDepthHighZBindings[i], "outputDepth1"_h,
				highZRenderTarget->GetMipLayer(glm::min(i + 2u, highZRenderTarget->GetMipLevels() - 1u)), 2);
			driver->AddStorageImageToShaderBindings(m_computeDepthHighZBindings[i], "outputDepth2"_h,
				highZRenderTarget->GetMipLayer(glm::min(i + 3u, highZRenderTarget->GetMipLevels() - 1u)), 3);
		}

		m_computePrepassDepthHighZBindings = driver->CreateShaderBindings();
		driver->AddSamplerToShaderBindings(m_computePrepassDepthHighZBindings, "inputDepth"_h, depthAttachment->GetDepthAspect(), 0);
		driver->AddStorageImageToShaderBindings(m_computePrepassDepthHighZBindings, "outputDepth"_h, highZRenderTarget->GetMipLayer(0), 1);
		driver->AddStorageImageToShaderBindings(m_computePrepassDepthHighZBindings, "outputDepth1"_h,
			highZRenderTarget->GetMipLayer(glm::min(1u, highZRenderTarget->GetMipLevels() - 1u)), 2);
		driver->AddStorageImageToShaderBindings(m_computePrepassDepthHighZBindings, "outputDepth2"_h,
			highZRenderTarget->GetMipLayer(glm::min(2u, highZRenderTarget->GetMipLevels() - 1u)), 3);
		m_boundDepth = depthAttachment;
		m_boundPyramid = highZRenderTarget;
	}

	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdCompute);
	{
		commands->ImageMemoryBarrier(commandList, highZRenderTarget, EImageLayout::General);

		//Depth Downscale
		for (int32_t i = -1; i < (int32_t)highZRenderTarget->GetMipLevels() - 1;)
		{
			const bool bFirst = i == -1;

			auto readMipLevel = bFirst ? depthAttachment : highZRenderTarget->GetMipLayer(i);
			auto writeMipLevel = highZRenderTarget->GetMipLayer(i + 1);

			const glm::uvec2 mipSize = glm::uvec2(writeMipLevel->GetExtent().x, writeMipLevel->GetExtent().y);

			PushConstantsDownscale params{};
			params.m_outputSize = glm::vec2(mipSize.x, mipSize.y);
			uint32_t mipCount = 1u;
			auto nextSize = mipSize;
			// Only exact 2x2 reductions fit within a workgroup's tile. Odd
			// footprints overlap neighbouring tiles and retain the full shader path.
			while (mipCount < 3u && uint32_t(i + 1) + mipCount < highZRenderTarget->GetMipLevels() &&
				nextSize.x % 2u == 0u && nextSize.y % 2u == 0u)
			{
				nextSize /= 2u;
				++mipCount;
			}
			params.m_mipCountAndSamples = glm::uvec2(mipCount, uint32_t(readMipLevel->GetMsaaSamples()));

			if (bFirst)
			{
				commands->ImageMemoryBarrierForComputeSampling(commandList, readMipLevel);
			}
			else
			{
				commands->ImageMemoryBarrier(commandList, readMipLevel, EImageLayout::ComputeRead);
			}
			for (uint32_t output = 0u; output < mipCount; ++output)
			{
				commands->ImageMemoryBarrier(commandList, highZRenderTarget->GetMipLayer(uint32_t(i + 1) + output), EImageLayout::ComputeWrite);
			}

			commands->Dispatch(commandList,
				(bFirst ? inputShader : m_pComputeDepthHighZShader)->GetComputeShaderRHI(),
				(uint32_t)glm::ceil(float(mipSize.x) / 8),
				(uint32_t)glm::ceil(float(mipSize.y) / 8),
				1u,
				{ bFirst ? m_computePrepassDepthHighZBindings : m_computeDepthHighZBindings[i] },
				&params, sizeof(PushConstantsDownscale));
			i += int32_t(mipCount);
		}
	}
	// The cached MSAA image is reused by the ordinary scene passes.
	commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetDefaultLayout());
	commands->EndDebugRegion(commandList);
	frameGraph->MarkCurrentDepthPyramid(highZRenderTarget);
}

void DepthHighZNode::Clear()
{
	m_computeDepthHighZBindings.Clear();
	m_computePrepassDepthHighZBindings.Clear();
	m_boundDepth.Clear();
	m_boundPyramid.Clear();
}
