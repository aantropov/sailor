#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"

namespace Sailor::Framegraph
{
	class PostProcessNode final : public TFrameGraphNode<PostProcessNode>
	{
	public:
		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

	protected:

		static const char* m_name;

		ShaderSetPtr m_pShader{};
		RHI::RHIMaterialPtr m_postEffectMaterial{};
		RHI::RHIShaderBindingSetPtr m_shaderBindings{};
	};

	template class TFrameGraphNode<PostProcessNode>;
};
