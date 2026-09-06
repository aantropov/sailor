#pragma once
#include "FrameGraph/FrameGraphNode.h"

namespace Sailor::Framegraph
{
	// Analytic height fog, composited once over opaque/masked HDR colour.
	// fog = density (1/m), height falloff (1/m), base height (m), start distance (m).
	// scattering = single-scattering albedo, anisotropy, maximum opacity, transition seconds.
	class AtmosphericFogNode final : public TFrameGraphNode<AtmosphericFogNode>
	{
	public:
		struct alignas(16) ShaderParameters
		{
			glm::vec4 m_fog{};
			glm::vec4 m_scattering{};
			glm::vec4 m_directionToSun{};
			glm::vec4 m_sunIlluminance{};
			glm::vec4 m_previousDirectionToSun{};
			glm::vec4 m_previousSunIlluminance{};
		};
		static_assert(sizeof(ShaderParameters) == 96);
		// Render-thread diagnostics, or after Renderer::WaitIdle().
		const ShaderParameters& GetLightingParameters() const { return m_parameters; }
		SAILOR_API AtmosphericFogNode();
		SAILOR_API static const char* GetName() { return m_name; }
		SAILOR_API void PreloadShader();
		SAILOR_API bool IsShaderReady() const;
		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph,
			RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList,
			const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

	private:
		SAILOR_SHARED_API static const char* m_name;
		ShaderSetPtr m_shader;
		RHI::RHIMaterialPtr m_material;
		RHI::RHIShaderBindingSetPtr m_bindings;
		RHI::RHITexturePtr m_depthTexture;
		RHI::RHICubemapPtr m_environment, m_previousEnvironment;
		ShaderParameters m_parameters{};
		float m_lightingBlend = 1.0f;
		bool m_bMultisampling = false;
	};

	template class TFrameGraphNode<AtmosphericFogNode>;
}
