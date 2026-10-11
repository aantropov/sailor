#pragma once

#include "FrameGraph/PostProcessNode.h"
#include "RHI/MotionHistory.h"
#include "RHI/RenderSubmission.h"

namespace Sailor::Framegraph
{
	class MotionBlurNode final : public PostProcessNode
	{
	public:
		SAILOR_API MotionBlurNode();
		SAILOR_API static StringHash GetName() { return "MotionBlur"_h; }
		std::string_view GetDebugName() const override { return GetName().ToString(); }
		SAILOR_API Tasks::TaskPtr<> Prepare(RHI::RHIFrameGraphPtr frameGraph, RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API void Process(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList,
			RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API void Clear() override;

	private:
		struct CameraHistory
		{
			TSharedPtr<const RHI::RHIMotionHistoryFrame> m_frame;
			RHI::RHISubmissionCompletionTokenPtr m_completion;
		};
		TVector<CameraHistory> m_history;
	};
}
