#pragma once

#include "Core/Defines.h"
#include "FrameGraph/BlitNode.h"
#include "FrameGraph/FrameGraphNode.h"
#include "FrameGraph/PostProcessNode.h"
#include "Memory/RefPtr.hpp"
#include "RHI/RenderDebugView.h"

#include <array>

namespace Sailor::Framegraph
{
	class DebugViewNode final : public TFrameGraphNode<DebugViewNode>
	{
	public:
		SAILOR_API static StringHash GetName() { return "DebugView"_h; }

		SAILOR_API virtual Sailor::Tasks::TaskPtr<void, void> Prepare(
			RHI::RHIFrameGraphPtr frameGraph,
			RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Process(
			RHI::RHIFrameGraphPtr frameGraph,
			RHI::RHICommandListPtr transferCommandList,
			RHI::RHICommandListPtr commandList,
			const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

	private:
		void EnsurePasses();
		void CopyResource(
			BaseFrameGraphNode& destination,
			StringHash destinationName,
			StringHash sourceName);
		PostProcessNode* GetDebugPass(RHI::ESceneViewRenderMode mode);

		TRefPtr<BlitNode> m_litPass{};
		std::array<TRefPtr<PostProcessNode>, 3> m_debugPasses{};
		uint64_t m_appliedParameterRevision = 0;
	};

	template class TFrameGraphNode<DebugViewNode>;
}
