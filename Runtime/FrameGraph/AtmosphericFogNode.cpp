#include "AtmosphericFogNode.h"
#include "AssetRegistry/AssetRegistry.h"
#include "FrameGraph/EnvironmentNode.h"
#include "Raytracing/SkyEnvironmentGenerator.h"
#include "RHI/Cubemap.h"
#include "RHI/RenderTarget.h"
#include "RHI/SceneView.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/VertexDescription.h"
#include <cmath>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* AtmosphericFogNode::m_name = "AtmosphericFog";
#endif

AtmosphericFogNode::AtmosphericFogNode()
{
	SetVec4("fog", glm::vec4(0.0f));
	SetVec4("scattering", glm::vec4(0.9f, 0.3f, 0.95f, 0.35f));
}

void AtmosphericFogNode::PreloadShader()
{
	if (!m_shader)
	{
		if (const auto info = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/AtmosphericFog.shader"))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader(info->GetFileId(), m_shader);
		}
	}
}

bool AtmosphericFogNode::IsShaderReady() const
{
	return m_shader && m_shader->IsReady();
}

void AtmosphericFogNode::Process(RHIFrameGraphPtr frameGraph, RHICommandListPtr transferCommandList,
	RHICommandListPtr commandList, const RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	ResetDrawCallStats();

	m_parameters.m_fog = GetVec4("fog");
	m_parameters.m_scattering = GetVec4("scattering");
	for (uint32_t i = 0; i < 4; ++i)
	{
		if (!std::isfinite(m_parameters.m_fog[i]) || !std::isfinite(m_parameters.m_scattering[i])) return;
	}
	if (m_parameters.m_fog.x <= 0.0f || m_parameters.m_scattering.z <= 0.0f || !frameGraph) return;

	RHITexturePtr color = GetResolvedAttachment("color");
	RHISurfacePtr surface = GetRHIResource("color").DynamicCast<RHISurface>();
	RHITexturePtr depth = GetResolvedAttachment("depthSampler");
	for (const auto& resource : m_unresolvedResourceParams)
	{
		if (resource.First() == "color")
		{
			surface = frameGraph->GetSurface(*resource.Second());
			color = surface ? surface->GetResolved() : frameGraph->GetRenderTarget(*resource.Second());
		}
		else if (resource.First() == "depthSampler") depth = frameGraph->GetRenderTarget(*resource.Second());
	}
	if (!color || !depth || !sceneView.m_frameBindings) return;
	PreloadShader();
	if (!IsShaderReady()) return;
	const auto environment = frameGraph->GetSampler("g_irradianceCubemap").DynamicCast<RHICubemap>();
	if (!environment) return;
	const float transitionSeconds = glm::max(m_parameters.m_scattering.w, 0.0f);
	const float deltaTime = std::isfinite(sceneView.m_deltaTime) ? glm::max(sceneView.m_deltaTime, 0.0f) : 0.0f;
	m_lightingBlend = transitionSeconds > 0.0f
		? glm::min(1.0f, m_lightingBlend + deltaTime / transitionSeconds) : 1.0f;
	bool lightingChanged = false;
	// Finish the current fade before adopting the newest generation. This keeps
	// transitions continuous even if the producer updates faster than the fade.
	if (environment != m_environment && m_lightingBlend >= 1.0f)
	{
		m_previousEnvironment = m_environment;
		m_parameters.m_previousDirectionToSun = m_parameters.m_directionToSun;
		m_parameters.m_previousSunIlluminance = m_parameters.m_sunIlluminance;
		m_environment = environment;
		m_parameters.m_directionToSun = glm::vec4(0, 1, 0, 0);
		m_parameters.m_sunIlluminance = glm::vec4(0);
		SkyParameters sky;
		if (const auto node = frameGraph->GetGraphNode("Environment").DynamicCast<EnvironmentNode>();
			node && node->GetEnvironmentSkyParams(sky))
		{
			m_parameters.m_directionToSun = glm::vec4(Math::SafeNormalize(-glm::vec3(sky.m_lightDirection),
				Math::vec3_Up), m_parameters.m_directionToSun.w);
			m_parameters.m_sunIlluminance = glm::vec4(Raytracing::CalculateDirectSunIlluminance(sky), 0);
		}
		m_lightingBlend = m_previousEnvironment && transitionSeconds > 0.0f ? 0.0f : 1.0f;
		lightingChanged = true;
	}
	if (m_lightingBlend >= 1.0f && m_previousEnvironment != m_environment)
	{
		m_previousEnvironment = m_environment;
		m_parameters.m_previousDirectionToSun = m_parameters.m_directionToSun;
		m_parameters.m_previousSunIlluminance = m_parameters.m_sunIlluminance;
		lightingChanged = true;
	}
	m_parameters.m_scattering.w = m_lightingBlend;

	auto& driver = App::GetSubmodule<Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<Renderer>()->GetDriverCommands();
	RHITexturePtr sampledDepth = depth;
	if (const auto target = depth.DynamicCast<RHIRenderTarget>())
	{
		if (const auto aspect = target->GetDepthAspect()) sampledDepth = aspect;
	}
	if (!m_bindings || m_depthTexture != sampledDepth || lightingChanged)
	{
		m_bindings = driver->CreateShaderBindings();
		driver->FillShadersLayout(m_bindings,
			{ m_shader->GetDebugVertexShaderRHI(), m_shader->GetDebugFragmentShaderRHI() }, 1);
		driver->AddBufferToShaderBindings(m_bindings, "data", sizeof(ShaderParameters), 0, EShaderBindingType::UniformBuffer);
		driver->AddSamplerToShaderBindings(m_bindings, "depthSampler", sampledDepth, 1);
		driver->AddSamplerToShaderBindings(m_bindings, "environmentSampler", m_environment, 2);
		driver->AddSamplerToShaderBindings(m_bindings, "previousEnvironmentSampler", m_previousEnvironment, 3);
		m_bindings->RecalculateCompatibility();
		m_depthTexture = sampledDepth;
	}
	commands->UpdateShaderBinding(transferCommandList, m_bindings->GetOrAddShaderBinding("data"), &m_parameters, sizeof(m_parameters));
	const bool multisampling = surface && surface->NeedsResolve();
	if (!m_material || m_bMultisampling != multisampling)
	{
		m_bMultisampling = multisampling;
		const RenderState state{ false, false, 0, false, ECullMode::None,
			EBlendMode::AlphaBlendingPreserveAlpha, EFillMode::Fill, 0, multisampling };
		m_material = driver->CreateMaterial(driver->GetOrAddVertexDescription<VertexP3N3UV2C4>(),
			EPrimitiveTopology::TriangleList, state, m_shader);
	}

	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdPostProcess);
	commands->ImageMemoryBarrier(commandList, depth, EImageLayout::ShaderReadOnlyOptimal);
	commands->ImageMemoryBarrier(commandList, m_environment, EImageLayout::ShaderReadOnlyOptimal);
	commands->ImageMemoryBarrier(commandList, m_previousEnvironment, EImageLayout::ShaderReadOnlyOptimal);
	commands->ImageMemoryBarrier(commandList, color, EImageLayout::ColorAttachmentOptimal);
	const glm::vec4 viewport(0, 0, color->GetExtent().x, color->GetExtent().y);
	if (multisampling)
	{
		// Blend into the live MSAA surface as well as its resolve, so a later
		// transparent pass cannot restore an unfogged multisampled background.
		commands->ImageMemoryBarrier(commandList, surface->GetTarget(), EImageLayout::ColorAttachmentOptimal);
		commands->BeginRenderPass(commandList, TVector<RHISurfacePtr>{surface}, nullptr,
			viewport, glm::ivec2(0), false, glm::vec4(0), 0.0f, false);
	}
	else
	{
		commands->BeginRenderPass(commandList, TVector<RHITexturePtr>{color}, nullptr,
			viewport, glm::ivec2(0), false, glm::vec4(0), 0.0f, false);
	}
	const auto mesh = frameGraph->GetFullscreenNdcQuad();
	commands->BindMaterial(commandList, m_material);
	commands->BindVertexBuffer(commandList, mesh->m_vertexBuffer, 0);
	commands->BindIndexBuffer(commandList, mesh->m_indexBuffer, 0);
	commands->BindShaderBindings(commandList, m_material, { sceneView.m_frameBindings, m_bindings });
	commands->SetViewport(commandList, 0, 0, viewport.z, viewport.w,
		glm::vec2(0), glm::vec2(viewport.z, viewport.w), 0, 1.0f);
	commands->DrawIndexed(commandList, 6, 1,
		uint32_t(mesh->m_indexBuffer->GetOffset() / sizeof(uint32_t)),
		uint32_t(mesh->m_vertexBuffer->GetOffset() / mesh->m_vertexDescription->GetVertexStride()), 0);
	RecordDrawCallStats(1);
	commands->EndRenderPass(commandList);
	commands->EndDebugRegion(commandList);
}

void AtmosphericFogNode::Clear()
{
	m_shader.Clear();
	m_material.Clear();
	m_bindings.Clear();
	m_depthTexture.Clear();
	m_environment.Clear();
	m_previousEnvironment.Clear();
	m_parameters = {};
	m_lightingBlend = 1.0f;
}
