#include "BlitNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/RenderTarget.h"
#include "RHI/Texture.h"
#include "RHI/VertexDescription.h"
#include "Engine/World.h"
#include "Engine/GameObject.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* BlitNode::m_name = "Blit";
#endif

void BlitNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdTransfer);

	if (!m_pShader)
	{
		const std::string shaderPath = "Shaders/Blit.shader";

		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(shaderPath))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetFileId(), m_pShader, {});
		}
	}

	if (!m_pShader || !m_pShader->IsReady())
	{
		return;
	}

	if (!m_blitToMsaaTargetMaterial)
	{
		m_shaderBindings = driver->CreateShaderBindings();
		RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
		RenderState renderState{ false, false, 0, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, 0, true };
		m_blitToMsaaTargetMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pShader, m_shaderBindings);
	}

	RHI::RHITexturePtr src = GetResolvedAttachment("src");
	RHI::RHITexturePtr dst = GetResolvedAttachment("dst");

	for (auto& r : m_unresolvedResourceParams)
	{
		if (r.First() == "src")
		{
			src = frameGraph->GetRenderTarget(r.Second());
		}
		else if (r.First() == "dst")
		{
			dst = frameGraph->GetRenderTarget(r.Second());
		}
	}

	//glm::vec4 srcRegion = GetVec4("srcRegion");
	//glm::vec4 dstRegion = GetVec4("dstRegion");
	glm::ivec4 srcRegion(0, 0, src->GetExtent().x, src->GetExtent().y);
	glm::ivec4 dstRegion(0, 0, dst->GetExtent().x, dst->GetExtent().y);

	commands->ImageMemoryBarrier(commandList, src, src->GetFormat(), src->GetDefaultLayout(), RHI::EImageLayout::TransferSrcOptimal);
	commands->ImageMemoryBarrier(commandList, dst, dst->GetFormat(), dst->GetDefaultLayout(), RHI::EImageLayout::TransferDstOptimal);

	const bool bIsDepthFormat = RHI::IsDepthFormat(src->GetFormat()) || RHI::IsDepthFormat(dst->GetFormat());

	commands->BlitImage(commandList, src, dst, srcRegion, dstRegion, bIsDepthFormat ? ETextureFiltration::Nearest : ETextureFiltration::Linear);

	commands->ImageMemoryBarrier(commandList, src, src->GetFormat(), RHI::EImageLayout::TransferSrcOptimal, src->GetDefaultLayout());
	commands->ImageMemoryBarrier(commandList, dst, dst->GetFormat(), RHI::EImageLayout::TransferDstOptimal, dst->GetDefaultLayout());

	// Blit to MSAA targets
	RHISurfacePtr dstSurface = GetRHIResource("dst").DynamicCast<RHISurface>();
	if (dstSurface && dstSurface->NeedsResolve())
	{
		bool bBlitIsSuccesful = false;

		// First try to blit MSAA src to MSAA dst
		if (RHISurfacePtr srcSurface = GetRHIResource("src").DynamicCast<RHISurface>())
		{
			auto src2 = srcSurface->GetTarget();
			auto dst2 = dstSurface->GetTarget();

			if (srcSurface->NeedsResolve())
			{
				commands->ImageMemoryBarrier(commandList, src2, src2->GetFormat(), src2->GetDefaultLayout(), RHI::EImageLayout::TransferSrcOptimal);
				commands->ImageMemoryBarrier(commandList, dst2, dst2->GetFormat(), dst2->GetDefaultLayout(), RHI::EImageLayout::TransferDstOptimal);

				bBlitIsSuccesful = commands->BlitImage(commandList, src2, dst2, srcRegion, dstRegion);

				commands->ImageMemoryBarrier(commandList, src2, src2->GetFormat(), RHI::EImageLayout::TransferSrcOptimal, src2->GetDefaultLayout());
				commands->ImageMemoryBarrier(commandList, dst2, dst2->GetFormat(), RHI::EImageLayout::TransferDstOptimal, dst2->GetDefaultLayout());
			}
		}

		// If no success blit texture src to MSAA dst
		if (!bBlitIsSuccesful)
		{
			auto target = dstSurface->GetTarget();

			// Should resolve MSAA
			commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), target->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);
			commands->ImageMemoryBarrier(commandList, src, src->GetFormat(), src->GetDefaultLayout(), RHI::EImageLayout::ShaderReadOnlyOptimal);

			BlitToSurface(commandList, frameGraph, sceneView, src, dstSurface);

			commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), target->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);
			commands->ImageMemoryBarrier(commandList, src, src->GetFormat(), RHI::EImageLayout::ShaderReadOnlyOptimal, src->GetDefaultLayout());
		}
	}

	commands->EndDebugRegion(commandList);
}

void BlitNode::BlitToSurface(RHI::RHICommandListPtr commandList,
	RHI::RHIFrameGraph* frameGraph,
	const RHI::RHISceneViewSnapshot& sceneView,
	RHI::RHITexturePtr src,
	RHI::RHISurfacePtr dst)
{
	SAILOR_PROFILE_FUNCTION();

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	driver->AddSamplerToShaderBindings(m_shaderBindings, "colorSampler", src, 0);
	m_shaderBindings->RecalculateCompatibility();

	auto target = dst->GetTarget();

	auto mesh = frameGraph->GetFullscreenNdcQuad();

	commands->BeginRenderPass(commandList,
		TVector<RHI::RHISurfacePtr>{dst},
		nullptr,
		glm::vec4(0, 0, target->GetExtent().x, target->GetExtent().y),
		glm::ivec2(0, 0),
		false,
		glm::vec4(0.0f),
		0.0f,
		false);

	const uint32_t firstIndex = (uint32_t)mesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
	const uint32_t vertexOffset = (uint32_t)mesh->m_vertexBuffer->GetOffset() / (uint32_t)mesh->m_vertexDescription->GetVertexStride();

	commands->BindMaterial(commandList, m_blitToMsaaTargetMaterial);
	commands->BindVertexBuffer(commandList, mesh->m_vertexBuffer, 0);
	commands->BindIndexBuffer(commandList, mesh->m_indexBuffer, 0);
	commands->BindShaderBindings(commandList, m_blitToMsaaTargetMaterial, { sceneView.m_frameBindings, m_shaderBindings });

	// TODO: Support regions
	commands->SetViewport(commandList,
		0, 0,
		(float)target->GetExtent().x, (float)target->GetExtent().y,
		glm::vec2(0, 0),
		glm::vec2(target->GetExtent().x, target->GetExtent().y),
		0, 1.0f);

	commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
	commands->EndRenderPass(commandList);
}

void BlitNode::Clear()
{
}
