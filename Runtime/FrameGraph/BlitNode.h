#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"

namespace Sailor::Framegraph
{
	class BlitNode : public TFrameGraphNode<BlitNode>
	{
	public:
		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraph* frameGraph,
			RHI::RHICommandListPtr transferCommandList,
			RHI::RHICommandListPtr commandList,
			const RHI::RHISceneViewSnapshot& sceneView) override;

		SAILOR_API virtual void Clear() override;

	protected:

		SAILOR_API void BlitToSurface(RHI::RHICommandListPtr commandList,
			RHI::RHIFrameGraph* frameGraph,
			const RHI::RHISceneViewSnapshot& sceneView,
			RHI::RHITexturePtr src,
			RHI::RHISurfacePtr dst);

		ShaderSetPtr m_pShader{};
		RHI::RHIMaterialPtr m_blitToMsaaTargetMaterial{};
		RHI::RHIShaderBindingSetPtr m_shaderBindings{};

		static const char* m_name;
	};

	template class TFrameGraphNode<BlitNode>;
};
