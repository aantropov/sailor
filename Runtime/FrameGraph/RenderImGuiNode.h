#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"

namespace Sailor::Framegraph
{
	class RenderImGuiNode : public TFrameGraphNode<RenderImGuiNode>
	{
	public:
		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override {}

	protected:

		static const char* m_name;
	};

	template class TFrameGraphNode<RenderImGuiNode>;
};
