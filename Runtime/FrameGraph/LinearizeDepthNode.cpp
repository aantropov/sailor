#include "LinearizeDepthNode.h"
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
const char* LinearizeDepthNode::m_name = "LinearizeDepth";
#endif

void LinearizeDepthNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdGraphics);

	auto depthAttachment = GetResolvedAttachment("depthStencil");
	for (auto& r : m_unresolvedResourceParams)
	{
		if (r.First() == "depthStencil")
		{
			depthAttachment = frameGraph->GetRenderTarget(r.Second());
			break;
		}
	}

	if (!m_pLinearizeDepthShader)
	{
		auto computeShaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/LinearizeDepth.shader");
		App::GetSubmodule<ShaderCompiler>()->LoadShader(computeShaderInfo->GetUID(), m_pLinearizeDepthShader);
	}

	auto target = GetResolvedAttachment("target");

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
		RenderState renderState{ false, false, 0, false, ECullMode::Back, EBlendMode::None, EFillMode::Fill, 0, false };
		m_postEffectMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pLinearizeDepthShader);
	}


	// How to correctly handle linearization
	// https://thxforthefish.com/posts/reverse_z/
	
	// struct PushConstants { mat4 m_invProjection; vec4 m_cameraParams; } constants;
	// constants.m_invProjection = sceneView.m_camera->GetInvProjection();
	
	// Standard projection matrix or ReverseZ projection with infinity far plane
	//constants.m_cameraParams = glm::vec4(sceneView.m_camera->GetZFar(), sceneView.m_camera->GetZNear(), 0, 0);

	// ReverseZ projection matrix
	//constants.m_cameraParams = glm::vec4(sceneView.m_camera->GetZFar(), sceneView.m_camera->GetZNear(), 0, 0);

	commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), depthAttachment->GetDefaultLayout(), EImageLayout::ShaderReadOnlyOptimal);
	commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), target->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);

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
	commands->SetDefaultViewport(commandList);
	commands->BindVertexBuffer(commandList, mesh->m_vertexBuffer, 0);
	commands->BindIndexBuffer(commandList, mesh->m_indexBuffer, 0);
	commands->BindShaderBindings(commandList, m_postEffectMaterial, { sceneView.m_frameBindings,  m_linearizeDepth });
	
	//commands->PushConstants(commandList, m_postEffectMaterial, sizeof(PushConstants), &constants);
	
	const uint32_t firstIndex = (uint32_t)mesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
	const uint32_t vertexOffset = (uint32_t)mesh->m_vertexBuffer->GetOffset() / (uint32_t)mesh->m_vertexDescription->GetVertexStride();

	commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
	commands->EndRenderPass(commandList);

	commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), EImageLayout::ColorAttachmentOptimal, target->GetDefaultLayout());
	commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), EImageLayout::ShaderReadOnlyOptimal, depthAttachment->GetDefaultLayout());

	commands->EndDebugRegion(commandList);
}

void LinearizeDepthNode::Clear()
{
	m_linearizeDepth.Clear();
	m_postEffectMaterial.Clear();
	m_pLinearizeDepthShader.Clear();
}
