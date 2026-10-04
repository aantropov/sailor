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

	auto resources = sceneView.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(this, sceneView.m_cameraIndex, 0);
	if (resources->m_shaderGeneration != m_shaderGeneration)
	{
		resources->m_shaderBindings.Clear();
		resources->m_uploadedParameterRevision = 0;
		resources->m_shaderGeneration = m_shaderGeneration;
	}
	auto& bindings = resources->m_shaderBindings;
	const bool bindingsCreated = !bindings;
	if (bindingsCreated)
	{
		bindings = driver->CreateShaderBindings();

		// Reflection retains uniform names in the debug bytecode; rendering uses the material's regular shaders.
		driver->FillShadersLayout(bindings, { m_pShader->GetDebugVertexShaderRHI(), m_pShader->GetDebugFragmentShaderRHI() }, 1);

		const auto layouts = bindings->GetLayoutBindings();
		for (const auto& layout : layouts)
		{
			if (layout.m_type == EShaderBindingType::UniformBuffer)
			{
				driver->AddBufferToShaderBindings(bindings, layout.m_name,
					(std::max)(layout.m_size, layout.m_paddedSize), layout.m_binding, layout.m_type);
			}
		}
	}

	if (!m_postEffectMaterial || m_bMultisampling != bShouldUseMsaaTarget)
	{
		m_bMultisampling = bShouldUseMsaaTarget;
		RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
		RenderState renderState{ false, false, 0, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, 0, bShouldUseMsaaTarget };
		m_postEffectMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pShader, bindings);
	}

	const bool parametersChanged = bindingsCreated || resources->m_uploadedParameterRevision != m_parameterRevision;
	for (const auto& binding : bindings->GetLayoutBindings())
	{
		if (binding.m_type == EShaderBindingType::CombinedImageSampler &&
			(parametersChanged || m_unresolvedResourceParams.ContainsKey(binding.m_name)) &&
			!driver->UpdateShaderBinding(bindings, binding.m_name, GetSampledAttachment(binding.m_name, frameGraph.GetRawPtr())))
		{
			commands->EndDebugRegion(commandList);
			return;
		}
	}

	if (parametersChanged)
	{
		for (const auto& v : m_vectorParams)
		{
			commands->SetMaterialParameter(transferCommandList, bindings, v.First(), *v.Second());
		}

		for (const auto& f : m_floatParams)
		{
			commands->SetMaterialParameter(transferCommandList, bindings, f.First(), *f.Second());
		}

		resources->m_uploadedParameterRevision = m_parameterRevision;
	}

	const auto& layout = bindings->GetLayoutBindings();

	{
		SAILOR_PROFILE_SCOPE("Image barriers");

		for (const auto& binding : layout)
		{
			if (binding.m_type == RHI::EShaderBindingType::CombinedImageSampler)
			{
				auto& shaderBinding = bindings->GetOrAddShaderBinding(binding.m_name);
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
	if (commands->BindShaderBindings(commandList, m_postEffectMaterial, { sceneView.m_frameBindings, bindings, sceneView.m_rhiLightsData }))
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
	++m_shaderGeneration;
}
