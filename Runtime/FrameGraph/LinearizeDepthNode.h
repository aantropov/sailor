#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"

namespace Sailor::Framegraph
{
	class LinearizeDepthNode : public TFrameGraphNode<LinearizeDepthNode>
	{
	public:
		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

	protected:

		struct PushConstants
		{
			mat4 m_invProjection;
			vec4 m_cameraParams; //cameraParams: (Znear, ZFar, 0, 0)
		};

		static const char* m_name;

		ShaderSetPtr m_pLinearizeDepthShader{};
		RHI::RHIMaterialPtr m_postEffectMaterial{};
		RHI::RHIShaderBindingSetPtr m_linearizeDepth{};
	};

	template class TFrameGraphNode<LinearizeDepthNode>;
};
