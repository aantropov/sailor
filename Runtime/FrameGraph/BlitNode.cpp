#include "BlitNode.h"
#include "BlitFormatConversion.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/RenderTarget.h"
#include "RHI/Texture.h"
#include "RHI/VertexDescription.h"
#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "AssetRegistry/AssetRegistry.h"

#include <cstdint>
#include <string_view>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* BlitNode::m_name = "Blit";
#endif

namespace
{
	enum class EFormatNumericClass : uint8_t
	{
		Unknown,
		Float,
		Normalized,
		Scaled,
		UnsignedInteger,
		SignedInteger,
		Srgb
	};

	EFormatNumericClass GetFormatNumericClass(RHI::ETextureFormat format)
	{
		const std::string_view name = magic_enum::enum_name(format);
		if (name.find("_SFLOAT") != std::string_view::npos ||
			name.find("_UFLOAT") != std::string_view::npos)
		{
			return EFormatNumericClass::Float;
		}
		if (name.find("_UNORM") != std::string_view::npos ||
			name.find("_SNORM") != std::string_view::npos)
		{
			return EFormatNumericClass::Normalized;
		}
		if (name.find("_USCALED") != std::string_view::npos ||
			name.find("_SSCALED") != std::string_view::npos)
		{
			return EFormatNumericClass::Scaled;
		}
		if (name.find("_UINT") != std::string_view::npos)
		{
			return EFormatNumericClass::UnsignedInteger;
		}
		if (name.find("_SINT") != std::string_view::npos)
		{
			return EFormatNumericClass::SignedInteger;
		}
		if (name.find("_SRGB") != std::string_view::npos)
		{
			return EFormatNumericClass::Srgb;
		}
		return EFormatNumericClass::Unknown;
	}
}

bool Sailor::Framegraph::RequiresShaderColorBlitForFormatConversion(
	RHI::ETextureFormat srcFormat,
	RHI::ETextureFormat dstFormat)
{
	if (RHI::IsDepthFormat(srcFormat) || RHI::IsDepthFormat(dstFormat))
	{
		return false;
	}

	const EFormatNumericClass srcClass = GetFormatNumericClass(srcFormat);
	const EFormatNumericClass dstClass = GetFormatNumericClass(dstFormat);
	return srcClass != EFormatNumericClass::Unknown &&
		dstClass != EFormatNumericClass::Unknown &&
		srcClass != dstClass;
}

void BlitNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	ResetDrawCallStats();

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

	if (m_pShader && m_pShader->IsReady() && !m_blitToMsaaTargetMaterial)
	{
		m_shaderBindings = driver->CreateShaderBindings();
		RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
		RenderState renderState{ false, false, 0, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, 0, true };
		m_blitToMsaaTargetMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pShader, m_shaderBindings);
	}

	RHI::RHITexturePtr src = GetResolvedAttachment("src");
	RHI::RHITexturePtr dst = GetResolvedAttachment("dst");

	for (const auto& r : m_unresolvedResourceParams)
	{
		if (r.First() == "src")
		{
			src = frameGraph->GetRenderTarget(*r.Second());
		}
		else if (r.First() == "dst")
		{
			dst = frameGraph->GetRenderTarget(*r.Second());
		}
	}

	const bool bIsDepthFormat = RHI::IsDepthFormat(src->GetFormat()) || RHI::IsDepthFormat(dst->GetFormat());
	const bool bForceShaderConversion =
		Framegraph::RequiresShaderColorBlitForFormatConversion(
			src->GetFormat(),
			dst->GetFormat());

	//glm::vec4 srcRegion = GetVec4("srcRegion");
	//glm::vec4 dstRegion = GetVec4("dstRegion");
	glm::ivec4 srcRegion(0, 0, src->GetExtent().x, src->GetExtent().y);
	glm::ivec4 dstRegion(0, 0, dst->GetExtent().x, dst->GetExtent().y);

	RHISurfacePtr dstSurface = GetRHIResource("dst").DynamicCast<RHISurface>();
	const bool bUseFullscreenColorBlit =
		!bIsDepthFormat &&
		!dstSurface &&
		m_blitToMsaaTargetMaterial &&
		(src->GetExtent() != dst->GetExtent() || bForceShaderConversion);
	bool bResolvedBlitSuccessful = false;
	if (bUseFullscreenColorBlit)
	{
		// A fullscreen pass owns the native output viewport explicitly. This keeps
		// resolution-scaled Scene View targets correctly fitted on MoltenVK instead
		// of relying on cross-format vkCmdBlitImage scaling.
		commands->ImageMemoryBarrier(commandList, src, RHI::EImageLayout::ShaderReadOnlyOptimal);
		commands->ImageMemoryBarrier(commandList, dst, RHI::EImageLayout::ColorAttachmentOptimal);
		BlitRaw(commandList, frameGraph, sceneView, src, dst);
		bResolvedBlitSuccessful = true;
	}
	else
	{
		commands->ImageMemoryBarrier(commandList, src, RHI::EImageLayout::TransferSrcOptimal);
		commands->ImageMemoryBarrier(commandList, dst, RHI::EImageLayout::TransferDstOptimal);
		bResolvedBlitSuccessful = commands->BlitImage(
			commandList,
			src,
			dst,
			srcRegion,
			dstRegion,
			bIsDepthFormat ?
				ETextureFiltration::Nearest :
				ETextureFiltration::Linear);
	}

	// Blit to MSAA targets
	if (dstSurface && dstSurface->NeedsResolve())
	{
		bool bMsaaBlitSuccessful = false;

		// First try to blit MSAA src to MSAA dst
		if (RHISurfacePtr srcSurface = GetRHIResource("src").DynamicCast<RHISurface>())
		{
			auto src2 = srcSurface->GetTarget();
			auto dst2 = dstSurface->GetTarget();

			if (srcSurface->NeedsResolve())
			{
				commands->ImageMemoryBarrier(commandList, src2, RHI::EImageLayout::TransferSrcOptimal);
				commands->ImageMemoryBarrier(commandList, dst2, RHI::EImageLayout::TransferDstOptimal);

				bMsaaBlitSuccessful = commands->BlitImage(commandList, src2, dst2, srcRegion, dstRegion);
			}
		}

		// If no success blit texture src to MSAA dst
		if (!bMsaaBlitSuccessful && m_blitToMsaaTargetMaterial)
		{
			auto target = dstSurface->GetTarget();

			// Should resolve MSAA
			commands->ImageMemoryBarrier(commandList, target, EImageLayout::ColorAttachmentOptimal);
			commands->ImageMemoryBarrier(commandList, src, RHI::EImageLayout::ShaderReadOnlyOptimal);

			BlitRaw(commandList, frameGraph, sceneView, src, dstSurface->GetTarget());
		}
	}

	std::string generateMips;
	if (bResolvedBlitSuccessful &&
		dst->HasMipMaps() &&
		TryGetString("GenerateMips", generateMips) &&
		generateMips == "true")
	{
		commands->GenerateMipMaps(commandList, dst);
	}

	commands->EndDebugRegion(commandList);
}

void BlitNode::BlitRaw(RHI::RHICommandListPtr commandList,
	RHI::RHIFrameGraphPtr frameGraph,
	const RHI::RHISceneViewSnapshot& sceneView,
	RHI::RHITexturePtr src,
	RHI::RHITexturePtr dst)
{
	SAILOR_PROFILE_FUNCTION();

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	driver->AddSamplerToShaderBindings(m_shaderBindings, "colorSampler", src, 0);
	m_shaderBindings->RecalculateCompatibility();

	auto mesh = frameGraph->GetFullscreenNdcQuad();

	commands->BeginRenderPass(commandList,
		TVector<RHI::RHITexturePtr>{dst},
		nullptr,
		glm::vec4(0, 0, dst->GetExtent().x, dst->GetExtent().y),
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
		(float)dst->GetExtent().x, (float)dst->GetExtent().y,
		glm::vec2(0, 0),
		glm::vec2(dst->GetExtent().x, dst->GetExtent().y),
		0, 1.0f);

	commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
	RecordDrawCallStats(1);
	commands->EndRenderPass(commandList);
}

void BlitNode::Clear()
{
}
