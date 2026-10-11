#pragma once

#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"

namespace Sailor::Framegraph
{
	class GlobalIlluminationResolveNode final :
		public TFrameGraphNode<GlobalIlluminationResolveNode>
	{
	public:
		SAILOR_API static StringHash GetName() { return "GlobalIlluminationResolve"_h; }

		SAILOR_API virtual void Process(
			RHI::RHIFrameGraphPtr frameGraph,
			RHI::RHICommandListPtr transferCommandList,
			RHI::RHICommandListPtr commandList,
			const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

	protected:
		ShaderSetPtr m_shader{};
		RHI::RHIShaderBindingSetPtr m_bindings{};
		RHI::RHITexturePtr m_depthTexture{};
		RHI::RHITexturePtr m_probeCellIndicesTexture{};
	};

	template class TFrameGraphNode<GlobalIlluminationResolveNode>;
}
