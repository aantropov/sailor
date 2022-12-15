#include "SkyNode.h"
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
const char* SkyNode::m_name = "Sky";
#endif

void SkyNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdPostProcess);

	if (!m_pSkyShader)
	{
		const std::string shaderPath = "Shaders/Sky.shader";

		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(shaderPath))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetUID(), m_pSkyShader, { "FILL" });
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetUID(), m_pSunShader, { "SUN" });
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetUID(), m_pComposeShader, { "COMPOSE" });
		}
	}

	if (!m_pSkyTexture)
	{
		m_pSkyTexture = driver->CreateRenderTarget(
			commandList,
			glm::ivec2(SkyResolution, SkyResolution),
			1,
			ETextureFormat::R16G16B16A16_SFLOAT,
			ETextureFiltration::Bicubic,
			ETextureClamping::Repeat,
			ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::Sampled_Bit | ETextureUsageBit::ColorAttachment_Bit);

		driver->SetDebugName(m_pSunTexture, "Sky");

		return;
	}

	if (!m_pSunTexture)
	{
		m_pSunTexture = driver->CreateRenderTarget(
			commandList,
			glm::ivec2(SkyResolution, SkyResolution),
			1,
			ETextureFormat::R16G16B16A16_SFLOAT,
			ETextureFiltration::Bicubic,
			ETextureClamping::Clamp,
			ETextureUsageBit::Sampled_Bit | ETextureUsageBit::ColorAttachment_Bit);

		driver->SetDebugName(m_pSunTexture, "Sun");

		return;
	}

	if (!m_pSkyShader || !m_pSkyShader->IsReady() ||
		!m_pSunShader || !m_pSunShader->IsReady() ||
		!m_pComposeShader || !m_pComposeShader->IsReady())
	{
		return;
	}

	if (!m_pSkyMaterial)
	{
		m_pShaderBindings = driver->CreateShaderBindings();

		// Firstly we must assign the correct layout
		driver->FillShadersLayout(m_pShaderBindings, { m_pSkyShader->GetDebugVertexShaderRHI(), m_pSkyShader->GetDebugFragmentShaderRHI() }, 1);

		// That should be enough to handle all the uniforms 
		const size_t uniformsSize = std::max(256ull, m_vectorParams.Num() * sizeof(glm::vec4));
		RHIShaderBindingPtr data = driver->AddBufferToShaderBindings(m_pShaderBindings, "data", uniformsSize, 0, RHI::EShaderBindingType::UniformBuffer);
		driver->AddSamplerToShaderBindings(m_pShaderBindings, "skySampler", m_pSkyTexture, 1);
		driver->AddSamplerToShaderBindings(m_pShaderBindings, "sunSampler", m_pSunTexture, 2);

		RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
		RenderState renderState{ false, false, 0, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, 0, false };
		m_pSkyMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pSkyShader, m_pShaderBindings);
		m_pSunMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pSunShader, m_pShaderBindings);
		m_pComposeMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pComposeShader, m_pShaderBindings);

		const glm::vec4 initLightDirection = normalize(glm::vec4(0, -1, 1, 0));
		commands->UpdateShaderBindingVariable(transferCommandList, data, "lightDirection", &initLightDirection, sizeof(glm::vec4));
	}

	RHI::RHITexturePtr target = GetResolvedAttachment("color");
	RHI::RHITexturePtr depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");

	auto mesh = frameGraph->GetFullscreenNdcQuad();

	commands->BindVertexBuffer(commandList, mesh->m_vertexBuffer, 0);
	commands->BindIndexBuffer(commandList, mesh->m_indexBuffer, 0);

	commands->BeginDebugRegion(commandList, "Sky", DebugContext::Color_CmdPostProcess);
	{
		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), depthAttachment->GetDefaultLayout(), EImageLayout::DepthAttachmentStencilReadOnlyOptimal);
		commands->ImageMemoryBarrier(commandList, m_pSkyTexture, m_pSkyTexture->GetFormat(), m_pSkyTexture->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);

		commands->BindMaterial(commandList, m_pSkyMaterial);
		commands->BindShaderBindings(commandList, m_pSkyMaterial, { sceneView.m_frameBindings, m_pShaderBindings });

		commands->SetViewport(commandList,
			0, 0,
			(float)m_pSkyTexture->GetExtent().x, (float)m_pSkyTexture->GetExtent().y,
			glm::vec2(0, 0),
			glm::vec2(m_pSkyTexture->GetExtent().x, m_pSkyTexture->GetExtent().y),
			0, 1.0f);

		commands->BeginRenderPass(commandList,
			TVector<RHI::RHITexturePtr>{m_pSkyTexture},
			depthAttachment,
			glm::vec4(0, 0, m_pSkyTexture->GetExtent().x, m_pSkyTexture->GetExtent().y),
			glm::ivec2(0, 0),
			false,
			glm::vec4(0.0f),
			false);

		const uint32_t firstIndex = (uint32_t)mesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
		const uint32_t vertexOffset = (uint32_t)mesh->m_vertexBuffer->GetOffset() / (uint32_t)mesh->m_vertexDescription->GetVertexStride();

		commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
		commands->EndRenderPass(commandList);

		commands->ImageMemoryBarrier(commandList, m_pSkyTexture, m_pSkyTexture->GetFormat(), EImageLayout::ColorAttachmentOptimal, m_pSkyTexture->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), EImageLayout::DepthAttachmentStencilReadOnlyOptimal, depthAttachment->GetDefaultLayout());
	}
	commands->EndDebugRegion(commandList);

	commands->BeginDebugRegion(commandList, "Sun", DebugContext::Color_CmdPostProcess);
	{
		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), depthAttachment->GetDefaultLayout(), EImageLayout::DepthAttachmentStencilReadOnlyOptimal);
		commands->ImageMemoryBarrier(commandList, m_pSunTexture, m_pSunTexture->GetFormat(), m_pSunTexture->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);

		commands->BindMaterial(commandList, m_pSunMaterial);
		commands->BindShaderBindings(commandList, m_pSunMaterial, { sceneView.m_frameBindings, m_pShaderBindings });

		commands->SetViewport(commandList,
			0, 0,
			(float)m_pSunTexture->GetExtent().x, (float)m_pSunTexture->GetExtent().y,
			glm::vec2(0, 0),
			glm::vec2(m_pSunTexture->GetExtent().x, m_pSunTexture->GetExtent().y),
			0, 1.0f);

		commands->BeginRenderPass(commandList,
			TVector<RHI::RHITexturePtr>{m_pSunTexture},
			depthAttachment,
			glm::vec4(0, 0, m_pSunTexture->GetExtent().x, m_pSunTexture->GetExtent().y),
			glm::ivec2(0, 0),
			false,
			glm::vec4(0.0f),
			false);

		const uint32_t firstIndex = (uint32_t)mesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
		const uint32_t vertexOffset = (uint32_t)mesh->m_vertexBuffer->GetOffset() / (uint32_t)mesh->m_vertexDescription->GetVertexStride();

		commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
		commands->EndRenderPass(commandList);

		commands->ImageMemoryBarrier(commandList, m_pSunTexture, m_pSunTexture->GetFormat(), EImageLayout::ColorAttachmentOptimal, m_pSunTexture->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), EImageLayout::DepthAttachmentStencilReadOnlyOptimal, depthAttachment->GetDefaultLayout());
	}
	commands->EndDebugRegion(commandList);


	commands->BeginDebugRegion(commandList, "Compose", DebugContext::Color_CmdPostProcess);
	{
		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), depthAttachment->GetDefaultLayout(), EImageLayout::DepthAttachmentStencilReadOnlyOptimal);
		commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), target->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);
		commands->ImageMemoryBarrier(commandList, m_pSkyTexture, m_pSkyTexture->GetFormat(), m_pSkyTexture->GetDefaultLayout(), EImageLayout::ShaderReadOnlyOptimal);
		commands->ImageMemoryBarrier(commandList, m_pSunTexture, m_pSunTexture->GetFormat(), m_pSunTexture->GetDefaultLayout(), EImageLayout::ShaderReadOnlyOptimal);

		commands->BindMaterial(commandList, m_pComposeMaterial);
		commands->BindShaderBindings(commandList, m_pComposeMaterial, { sceneView.m_frameBindings, m_pShaderBindings });

		commands->SetViewport(commandList,
			0, 0,
			(float)target->GetExtent().x, (float)target->GetExtent().y,
			glm::vec2(0, 0),
			glm::vec2(target->GetExtent().x, target->GetExtent().y),
			0, 1.0f);

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

		commands->ImageMemoryBarrier(commandList, m_pSkyTexture, m_pSkyTexture->GetFormat(), EImageLayout::ShaderReadOnlyOptimal, m_pSkyTexture->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, m_pSunTexture, m_pSunTexture->GetFormat(), EImageLayout::ShaderReadOnlyOptimal, m_pSunTexture->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), EImageLayout::ColorAttachmentOptimal, target->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), EImageLayout::DepthAttachmentStencilReadOnlyOptimal, depthAttachment->GetDefaultLayout());
	}
	commands->EndDebugRegion(commandList);

	commands->EndDebugRegion(commandList);
}

void SkyNode::Clear()
{
	m_pSkyTexture.Clear();
	m_pSunTexture.Clear();
	m_pSkyShader.Clear();
	m_pSkyMaterial.Clear();
	m_pShaderBindings.Clear();
}
