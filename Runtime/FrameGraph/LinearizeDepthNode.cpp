#include "LinearizeDepthNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "RHI/Types.h"
#include "RHI/VertexDescription.h"
#include "AssetRegistry/AssetRegistry.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

void LinearizeDepthNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	ResetDrawCallStats();

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	auto depthAttachment = GetResolvedAttachment("depthStencil"_h, frameGraph.GetRawPtr());

	if (!m_pLinearizeDepthShader)
	{
		auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/LinearizeDepth.shader");
		App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetFileId(), m_pLinearizeDepthShader);
	}

	auto target = GetResolvedAttachment("target"_h, frameGraph.GetRawPtr());

	if (!m_pLinearizeDepthShader || !depthAttachment || !target || !m_pLinearizeDepthShader->IsReady())
	{
		return;
	}

	auto sampledDepthAttachment = GetSampledAttachment("depthStencil"_h, frameGraph.GetRawPtr());

	if (!m_linearizeDepth || m_boundDepthAttachment != sampledDepthAttachment)
	{
		if (!m_linearizeDepth)
		{
			m_linearizeDepth = driver->CreateShaderBindings();
		}

		driver->AddSamplerToShaderBindings(m_linearizeDepth, "depthSampler"_h, sampledDepthAttachment, 0);
		m_linearizeDepth->RecalculateCompatibility();
		m_boundDepthAttachment = sampledDepthAttachment;
	}

	if (!m_postEffectMaterial)
	{
		RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
		RenderState renderState{ false, false, 0, false, ECullMode::Back, EBlendMode::None, EFillMode::Fill, 0, false };
		m_postEffectMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pLinearizeDepthShader);
	}

	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdGraphics);
	commands->ImageMemoryBarrier(commandList, depthAttachment, EImageLayout::ShaderReadOnlyOptimal);
	commands->ImageMemoryBarrier(commandList, target, EImageLayout::ColorAttachmentOptimal);

	auto mesh = frameGraph->GetFullscreenNdcQuad();

	commands->BeginRenderPass(commandList,
		TVector<RHI::RHITexturePtr>{target},
		nullptr,
		glm::vec4(0, 0, target->GetExtent().x, target->GetExtent().y),
		glm::ivec2(0, 0),
		false,
		glm::vec4(0.0f),
		0.0f,
		false);

	commands->BindMaterial(commandList, m_postEffectMaterial);
	commands->SetViewport(commandList,
		0, (float)target->GetExtent().y,
		(float)target->GetExtent().x, -(float)target->GetExtent().y,
		glm::vec2(0, 0),
		glm::vec2(target->GetExtent().x, target->GetExtent().y),
		0, 1.0f);
	commands->BindVertexBuffer(commandList, mesh->m_vertexBuffer, 0);
	commands->BindIndexBuffer(commandList, mesh->m_indexBuffer, 0);
	if (commands->BindShaderBindings(commandList, m_postEffectMaterial, { sceneView.m_frameBindings, m_linearizeDepth }))
	{
		const uint32_t firstIndex = (uint32_t)mesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
		const uint32_t vertexOffset = (uint32_t)mesh->m_vertexBuffer->GetOffset() / (uint32_t)mesh->m_vertexDescription->GetVertexStride();

		commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
		RecordDrawCallStats(1);
	}
	commands->EndRenderPass(commandList);

	commands->EndDebugRegion(commandList);
}

void LinearizeDepthNode::Clear()
{
	m_linearizeDepth.Clear();
	m_boundDepthAttachment.Clear();
	m_postEffectMaterial.Clear();
	m_pLinearizeDepthShader.Clear();
}
