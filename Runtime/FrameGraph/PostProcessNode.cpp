#include "PostProcessNode.h"
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
#include "AssetRegistry/AssetRegistry.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* PostProcessNode::m_name = "PostProcess";
#endif

void PostProcessNode::PreloadShader()
{
	const auto shaderPath = GetString("shader");
	std::string definesStr;
	TryGetString("defines", definesStr);
	if (m_shaderPath != shaderPath || m_shaderDefines != definesStr)
	{
		Clear();
		m_shaderPath = shaderPath;
		m_shaderDefines = definesStr;
	}
	if (m_pShader)
	{
		return;
	}

	check(!shaderPath.empty());

	TVector<std::string> defines = Sailor::Utils::SplitString(definesStr, " ");

	if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(shaderPath))
	{
		App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetFileId(), m_pShader, defines);
	}
}

bool PostProcessNode::IsShaderReady() const
{
	return m_pShader && m_pShader->IsReady();
}

void PostProcessNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	ResetDrawCallStats();

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	RHI::RHITexturePtr target = GetResolvedAttachment("color", frameGraph.GetRawPtr());
	RHI::RHISurfacePtr targetMsaa = GetRHIResource("color", frameGraph.GetRawPtr()).DynamicCast<RHISurface>();

	const bool bShouldUseMsaaTarget = targetMsaa.IsValid() && targetMsaa->NeedsResolve();

	if (!target && !m_unresolvedResourceParams.ContainsKey("color"))
	{
		target = frameGraph->GetRenderTarget("BackBuffer");
	}

	PreloadShader();

	if (!m_pShader || !m_pShader->IsReady() || !target)
	{
		return;
	}

	const std::string shaderName = std::string(GetName()) + ":" + GetString("shader");
	commands->BeginDebugRegion(commandList, shaderName, DebugContext::Color_CmdPostProcess);

	const bool bindingsCreated = !m_shaderBindings;
	if (bindingsCreated)
	{
		m_shaderBindings = driver->CreateShaderBindings();

		// Reflection retains uniform names in the debug bytecode; rendering uses the material's regular shaders.
		driver->FillShadersLayout(m_shaderBindings, { m_pShader->GetDebugVertexShaderRHI(), m_pShader->GetDebugFragmentShaderRHI() }, 1);

		const auto layouts = m_shaderBindings->GetLayoutBindings();
		for (const auto& layout : layouts)
		{
			if (layout.m_type == EShaderBindingType::UniformBuffer)
			{
				driver->AddBufferToShaderBindings(m_shaderBindings, layout.m_name,
					(std::max)(layout.m_size, layout.m_paddedSize), layout.m_binding, layout.m_type);
			}
		}
	}

	if (!m_postEffectMaterial || m_bMultisampling != bShouldUseMsaaTarget)
	{
		m_bMultisampling = bShouldUseMsaaTarget;
		RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
		RenderState renderState{ false, false, 0, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, 0, bShouldUseMsaaTarget };
		m_postEffectMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pShader, m_shaderBindings);
	}

	const bool parametersChanged = bindingsCreated || m_uploadedParameterRevision != m_parameterRevision;
	if (parametersChanged)
	{
		for (const auto& v : m_vectorParams)
		{
			commands->SetMaterialParameter(transferCommandList, m_shaderBindings, v.First(), *v.Second());
		}

		for (const auto& f : m_floatParams)
		{
			commands->SetMaterialParameter(transferCommandList, m_shaderBindings, f.First(), *f.Second());
		}

		m_uploadedParameterRevision = m_parameterRevision;
	}

	for (const auto& binding : m_shaderBindings->GetLayoutBindings())
	{
		if (binding.m_type == EShaderBindingType::CombinedImageSampler &&
			(parametersChanged || m_unresolvedResourceParams.ContainsKey(binding.m_name)))
		{
			driver->UpdateShaderBinding(m_shaderBindings, binding.m_name,
				GetSampledAttachment(binding.m_name, frameGraph.GetRawPtr()));
		}
	}

	const auto& layout = m_shaderBindings->GetLayoutBindings();

	{
		SAILOR_PROFILE_SCOPE("Image barriers");

		for (const auto& binding : layout)
		{
			if (binding.m_type == RHI::EShaderBindingType::CombinedImageSampler)
			{
				auto& shaderBinding = m_shaderBindings->GetOrAddShaderBinding(binding.m_name);
				if (shaderBinding->IsBind())
				{
					auto pTexture = shaderBinding->GetTextureBinding();
					commands->ImageMemoryBarrier(commandList, pTexture, EImageLayout::ShaderReadOnlyOptimal);
				}
			}
		}

		commands->ImageMemoryBarrier(commandList, target, EImageLayout::ColorAttachmentOptimal);
	}

	auto mesh = frameGraph->GetFullscreenNdcQuad();

	if (bShouldUseMsaaTarget)
	{
		commands->ImageMemoryBarrier(commandList, targetMsaa->GetTarget(), EImageLayout::ColorAttachmentOptimal);

		commands->BeginRenderPass(commandList,
			TVector<RHI::RHISurfacePtr>{targetMsaa},
			nullptr,
			glm::vec4(0, 0, target->GetExtent().x, target->GetExtent().y),
			glm::ivec2(0, 0),
			false,
			glm::vec4(0.0f),
			0.0f,
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
			0.0f,
			false);
	}

	const uint32_t firstIndex = (uint32_t)mesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
	const uint32_t vertexOffset = (uint32_t)mesh->m_vertexBuffer->GetOffset() / (uint32_t)mesh->m_vertexDescription->GetVertexStride();

	commands->BindMaterial(commandList, m_postEffectMaterial);
	commands->BindVertexBuffer(commandList, mesh->m_vertexBuffer, 0);
	commands->BindIndexBuffer(commandList, mesh->m_indexBuffer, 0);
	if (commands->BindShaderBindings(commandList, m_postEffectMaterial, { sceneView.m_frameBindings,  m_shaderBindings, sceneView.m_rhiLightsData }))
	{
		commands->SetViewport(commandList,
			0, 0,
			(float)target->GetExtent().x, (float)target->GetExtent().y,
			glm::vec2(0, 0),
			glm::vec2(target->GetExtent().x, target->GetExtent().y),
			0, 1.0f);

		commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
		RecordDrawCallStats(1);
	}
	commands->EndRenderPass(commandList);

	commands->EndDebugRegion(commandList);
}

void PostProcessNode::Clear()
{
	m_pShader.Clear();
	m_postEffectMaterial.Clear();
	m_shaderBindings.Clear();
	m_uploadedParameterRevision = 0;
}
