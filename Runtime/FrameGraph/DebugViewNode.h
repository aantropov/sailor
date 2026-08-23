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
		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual Sailor::Tasks::TaskPtr<void, void> Prepare(
			RHI::RHIFrameGraphPtr frameGraph,
			const RHI::RHISceneViewSnapshot& sceneView) override;
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
			const std::string& destinationName,
			const std::string& sourceName);
		PostProcessNode* GetDebugPass(RHI::ESceneViewRenderMode mode);

		static const char* m_name;
		TRefPtr<BlitNode> m_litPass{};
		std::array<TRefPtr<PostProcessNode>, 3> m_debugPasses{};
	};

	template class TFrameGraphNode<DebugViewNode>;
}
