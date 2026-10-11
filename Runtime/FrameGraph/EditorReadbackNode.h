#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "RHI/Readback.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"

namespace Sailor::Framegraph
{
	class EditorReadbackNode : public TFrameGraphNode<EditorReadbackNode>
	{
	public:
		SAILOR_API static StringHash GetName() { return "EditorReadback"_h; }
		SAILOR_API static TRefPtr<EditorReadbackNode> Find(RHI::RHIFrameGraph& graph);

		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

		// Render thread only; Main receives an immutable completed record by task.
		SAILOR_API RHI::ReadbackFramePtr TakeCompletedFrame();
		SAILOR_API RHI::RHITexturePtr GetTexture() const { return m_texture; }
		SAILOR_API const RHI::EditorReadbackStats& GetStats() const { return m_stats; }

	protected:
		TVector<TSharedPtr<RHI::ReadbackFrame>> m_readbacks;
		RHI::RHITexturePtr m_texture;
		uint64_t m_nextFrameIndex = 1;
		uint64_t m_publishedFrameIndex = 0;
		RHI::EditorReadbackStats m_stats;

	};

#ifdef _SAILOR_IMPORT_
	extern template class TFrameGraphNode<EditorReadbackNode>;
#else
	template class TFrameGraphNode<EditorReadbackNode>;
#endif
}
