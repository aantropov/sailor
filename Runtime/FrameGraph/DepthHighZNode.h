#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"

// The part of GPU culling. 
// Inspired by https://vkguide.dev/docs/gpudriven/compute_culling/

namespace Sailor::Framegraph
{
	class DepthHighZNode : public TFrameGraphNode<DepthHighZNode>
	{
	public:
		SAILOR_API static StringHash GetName() { return "DepthHighZ"_h; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

	protected:

		ShaderSetPtr m_pComputeDepthHighZShader{};
		ShaderSetPtr m_pComputeDepthHighZInputShader{};
		ShaderSetPtr m_pComputeDepthHighZMsaaShader{};
		TVector<RHI::RHIShaderBindingSetPtr> m_computeDepthHighZBindings{};
		RHI::RHIShaderBindingSetPtr m_computePrepassDepthHighZBindings{};
		RHI::RHIRenderTargetPtr m_boundDepth{};
		RHI::RHIRenderTargetPtr m_boundPyramid{};

		struct PushConstantsDownscale
		{
			glm::vec2  m_outputSize;			
		};
	};

	template class TFrameGraphNode<DepthHighZNode>;
};
