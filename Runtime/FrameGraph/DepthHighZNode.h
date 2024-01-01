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
		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

	protected:

		static const char* m_name;

		ShaderSetPtr m_pComputeDepthHighZShader{};
		TVector<RHI::RHIShaderBindingSetPtr> m_computeDepthHighZBindings{};
		RHI::RHIShaderBindingSetPtr m_computePrepassDepthHighZBindings{};

		struct PushConstantsDownscale
		{
			glm::vec2  m_outputSize;			
		};
	};

	template class TFrameGraphNode<DepthHighZNode>;
};
