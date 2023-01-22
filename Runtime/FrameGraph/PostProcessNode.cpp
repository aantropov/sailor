#include "PostProcessNode.h"
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
const char* PostProcessNode::m_name = "PostProcess";
#endif

void PostProcessNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	RHI::RHITexturePtr target = GetResolvedAttachment("color");
	RHI::RHISurfacePtr targetMsaa = GetRHIResource("color").DynamicCast<RHISurface>();

	const bool bShouldUseMsaaTarget = targetMsaa.IsValid() && targetMsaa->NeedsResolve();

	if (!target)
	{
		target = frameGraph->GetRenderTarget("BackBuffer");
	}

	if (!m_pShader)
	{
		auto shaderPath = GetString("shader");
		check(!shaderPath.empty());

		auto definesStr = GetString("defines");
		TVector<std::string> defines = Sailor::Utils::SplitString(definesStr, " ");

		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(shaderPath))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetUID(), m_pShader, defines);
		}
	}

	if (!m_pShader || !m_pShader->IsReady() || !target)
	{
		return;
	}

	const std::string shaderName = std::string(GetName()) + ":" + GetString("shader");
	commands->BeginDebugRegion(commandList, shaderName, DebugContext::Color_CmdPostProcess);

	if (!m_postEffectMaterial)
	{
		m_shaderBindings = driver->CreateShaderBindings();

		// Firstly we must assign the correct layout
		driver->FillShadersLayout(m_shaderBindings, { m_pShader->GetDebugVertexShaderRHI(), m_pShader->GetDebugFragmentShaderRHI() }, 1);

		// That should be enough to handle all the uniforms
		const size_t uniformsSize = std::max(256ull, m_vectorParams.Num() * sizeof(glm::vec4));
		driver->AddBufferToShaderBindings(m_shaderBindings, "data", uniformsSize, 0, RHI::EShaderBindingType::UniformBuffer);

		RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
		RenderState renderState{ false, false, 0, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, 0, bShouldUseMsaaTarget };
		m_postEffectMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pShader, m_shaderBindings);

		for (auto& v : m_vectorParams)
		{
			commands->SetMaterialParameter(transferCommandList, m_shaderBindings, v.First(), v.Second());
		}

		for (auto& r : m_resourceParams)
		{
			auto rhiTexture = GetResolvedAttachment(r.First());
			driver->UpdateShaderBinding(m_shaderBindings, r.First(), rhiTexture);
		}
	}

	SAILOR_PROFILE_BLOCK("Image barriers");

	const auto& layout = m_shaderBindings->GetLayoutBindings();
	for (const auto& binding : layout)
	{
		if (binding.m_type == RHI::EShaderBindingType::CombinedImageSampler)
		{
			auto& shaderBinding = m_shaderBindings->GetOrAddShaderBinding(binding.m_name);
			if (shaderBinding->IsBind())
			{
				auto pTexture = shaderBinding->GetTextureBinding();
				commands->ImageMemoryBarrier(commandList, pTexture, pTexture->GetFormat(), pTexture->GetDefaultLayout(), EImageLayout::ShaderReadOnlyOptimal);
			}
		}
	}

	commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), target->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);

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

	if (bShouldUseMsaaTarget)
	{
		commands->BeginRenderPass(commandList,
			TVector<RHI::RHISurfacePtr>{targetMsaa},
			nullptr,
			glm::vec4(0, 0, target->GetExtent().x, target->GetExtent().y),
			glm::ivec2(0, 0),
			false,
			glm::vec4(0.0f),
			false);
	}
	else
	{
		commands->BeginRenderPass(commandList,
			TVector<RHI::RHITexturePtr>{target},
			nullptr,
			glm::vec4(0, 0, target->GetExtent().x, target->GetExtent().y),
			glm::ivec2(0, 0),
			false,
			glm::vec4(0.0f),
			false);
	}

	const uint32_t firstIndex = (uint32_t)mesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
	const uint32_t vertexOffset = (uint32_t)mesh->m_vertexBuffer->GetOffset() / (uint32_t)mesh->m_vertexDescription->GetVertexStride();

	commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
	commands->EndRenderPass(commandList);

	SAILOR_PROFILE_BLOCK("Image barriers");

	for (const auto& binding : layout)
	{
		if (binding.m_type == RHI::EShaderBindingType::CombinedImageSampler)
		{
			auto& shaderBinding = m_shaderBindings->GetOrAddShaderBinding(binding.m_name);
			if (shaderBinding->IsBind())
			{
				auto pTexture = shaderBinding->GetTextureBinding();
				commands->ImageMemoryBarrier(commandList, pTexture, pTexture->GetFormat(), EImageLayout::ShaderReadOnlyOptimal, pTexture->GetDefaultLayout());
			}
		}
	}

	commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), EImageLayout::ColorAttachmentOptimal, target->GetDefaultLayout());

	commands->EndDebugRegion(commandList);
	SAILOR_PROFILE_END_BLOCK();
}

void PostProcessNode::Clear()
{
	m_pShader.Clear();
	m_postEffectMaterial.Clear();
	m_shaderBindings.Clear();
}
