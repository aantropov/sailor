#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"

namespace Sailor::Framegraph
{
	class BloomNode : public TFrameGraphNode<BloomNode>
	{
	public:
		SAILOR_API static StringHash GetName() { return "Bloom"_h; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

	protected:

		ShaderSetPtr m_pComputeDownscaleShader{};
		ShaderSetPtr m_pComputeUpscaleShader{};

		TVector<RHI::RHIShaderBindingSetPtr> m_computeDownscaleBindings{};
		TVector<RHI::RHIShaderBindingSetPtr> m_computeUpscaleBindings{};
		RHI::RHITexturePtr m_bloomTarget;
		RHI::RHITexturePtr m_averageLuminance;
		RHI::RHITexturePtr m_lensDirt;

		struct PushConstantsDownscale
		{
			glm::vec4  m_threshold; // x -> threshold, yzw -> (threshold - knee, 2.0 * knee, 0.25 / knee)
			uint32_t   m_bIsThresholdEnabled;
		};

		struct PushConstantsUpscale
		{
			uint32_t  m_mipLevel;
			float	  m_bloomIntensity;
			float	  m_dirtIntensity;
			float	  m_scatter;
		};
	};

	template class TFrameGraphNode<BloomNode>;
};
