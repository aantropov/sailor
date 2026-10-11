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
	class CopyTextureToRamNode : public TFrameGraphNode<CopyTextureToRamNode>
	{
	public:
		using CaptureTask = Tasks::TaskPtr<RHI::ReadbackFramePtr, RHI::ReadbackFramePtr>;

		SAILOR_API static StringHash GetName() { return "CopyTextureToRam"_h; }
		SAILOR_API virtual ~CopyTextureToRamNode() override;

		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

		// A distinct result for this request. Read only after IsFinished().
		SAILOR_API CaptureTask DoOneCapture();
		SAILOR_API void PollCaptures(); // Render queue only.

	protected:

		struct Capture
		{
			TSharedPtr<RHI::ReadbackFrame> m_frame;
			TVector<CaptureTask> m_requests;
		};
		TVector<CaptureTask> m_requests;
		TVector<Capture> m_captures;
	};

#ifdef _SAILOR_IMPORT_
	extern template class TFrameGraphNode<CopyTextureToRamNode>;
#else
	template class TFrameGraphNode<CopyTextureToRamNode>;
#endif
};
