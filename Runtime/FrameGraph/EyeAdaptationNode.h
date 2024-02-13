#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"
#include "PostProcessNode.h"

namespace Sailor::Framegraph
{
	class EyeAdaptationNode : public TFrameGraphNode<PostProcessNode>
	{
	public:

		const uint32_t HistogramShades = 256;

		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

	protected:

		static const char* m_name;

		float m_whitePointLum{};

		ShaderSetPtr m_pToneMappingShader{};
		ShaderSetPtr m_pComputeHistogramShader{};
		ShaderSetPtr m_pComputeAverageShader{};

		RHI::RHIMaterialPtr m_postEffectMaterial{};
		RHI::RHIShaderBindingSetPtr m_shaderBindings{};

		RHI::RHIShaderBindingSetPtr m_computeHistogramShaderBindings{};
		RHI::RHIShaderBindingSetPtr m_computeAverageShaderBindings{};

		RHI::RHITexturePtr m_averageLuminance;
	};

	template class TFrameGraphNode<EyeAdaptationNode>;
};
