#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"

namespace Sailor::Framegraph
{
	class EyeAdaptationNode : public TFrameGraphNode<EyeAdaptationNode>
	{
	public:

		const uint32_t HistogramShades = 256;

		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

	protected:

		static const char* m_name;

		ShaderSetPtr m_pComputeHistogramShader{};
		ShaderSetPtr m_pComputeAverageShader{};

		RHI::RHIShaderBindingSetPtr m_computeHistogramShaderBindings{};
		RHI::RHIShaderBindingSetPtr m_computeAverageShaderBindings{};
		RHI::RHITexturePtr m_averageLuminanceTarget{};
		bool m_bAverageLuminanceInitialized = false;
	};

#ifndef _SAILOR_IMPORT_
	template class TFrameGraphNode<EyeAdaptationNode>;
#else
	extern template class TFrameGraphNode<EyeAdaptationNode>;
#endif
}
