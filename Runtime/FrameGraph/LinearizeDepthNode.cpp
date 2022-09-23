#include "LinearizeDepthNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/Texture.h"
#include "RHI/Types.h"
#include "RHI/VertexDescription.h"
#include "Engine/World.h"
#include "Engine/GameObject.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* LinearizeDepthNode::m_name = "LinearizeDepth";
#endif

void LinearizeDepthNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	auto depthAttachment = GetResourceParam("depthStencil").DynamicCast<RHI::RHITexture>();
	if (!depthAttachment)
	{
		depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");
	}

	if (!m_pLinearizeDepthShader)
	{
		auto computeShaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/LinearizeDepth.shader");
		App::GetSubmodule<ShaderCompiler>()->LoadShader(computeShaderInfo->GetUID(), m_pLinearizeDepthShader);
	}

	auto target = GetResourceParam("target").DynamicCast<RHI::RHITexture>();

	if (!m_pLinearizeDepthShader || !target || !m_pLinearizeDepthShader->IsReady())
	{
		return;
	}

	if (!m_linearizeDepth)
	{
		m_linearizeDepth = driver->CreateShaderBindings();
		driver->AddSamplerToShaderBindings(m_linearizeDepth, "depthSampler", depthAttachment, 0);
	}

	if (!m_postEffectMaterial)
	{
		RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
		RenderState renderState{ false, false, 0, ECullMode::Back, EBlendMode::None, EFillMode::Fill, 0, false };
		m_postEffectMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pLinearizeDepthShader);
	}

	commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), depthAttachment->GetDefaultLayout(), EImageLayout::ShaderReadOnlyOptimal);
	commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), target->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);

	auto mesh = frameGraph->GetFullscreenNdcQuad();
	commands->BindMaterial(commandList, m_postEffectMaterial);
	commands->BindVertexBuffer(commandList, mesh->m_vertexBuffer, 0);
	commands->BindIndexBuffer(commandList, mesh->m_indexBuffer, 0);
	commands->BindShaderBindings(commandList, m_postEffectMaterial, { sceneView.m_frameBindings,  m_linearizeDepth });

	// TODO: Pipeline without depthAttachment
	commands->BeginRenderPass(commandList,
		TVector<RHI::RHITexturePtr>{target},
		depthAttachment,
		glm::vec4(0, 0, target->GetExtent().x, target->GetExtent().y),
		glm::ivec2(0, 0),
		true,
		glm::vec4(0.0f),
		true);

	const uint32_t firstIndex = (uint32_t)mesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
	const uint32_t vertexOffset = (uint32_t)mesh->m_vertexBuffer->GetOffset() / (uint32_t)mesh->m_vertexDescription->GetVertexStride();

	commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
	commands->EndRenderPass(commandList);

	commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), EImageLayout::ColorAttachmentOptimal, target->GetDefaultLayout());
	commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), EImageLayout::ShaderReadOnlyOptimal, depthAttachment->GetDefaultLayout());
}

void LinearizeDepthNode::Clear()
{
	m_linearizeDepth.Clear();
	m_postEffectMaterial.Clear();
	m_pLinearizeDepthShader.Clear();
}
