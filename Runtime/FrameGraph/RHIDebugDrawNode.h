#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"

namespace Sailor
{
	class RHIDebugDrawNode : public TFrameGraphNode<RHIDebugDrawNode>
	{
	public:
		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual void Process(RHIFrameGraph* frameGraph, TVector<RHI::RHICommandListPtr>& transferCommandLists, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

	protected:

		static const char* m_name;
	};

	template class TFrameGraphNode<RHIDebugDrawNode>;
};
