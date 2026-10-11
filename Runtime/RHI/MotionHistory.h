#pragma once

#include "RHI/Scene.h"
#include "RHI/RenderDebugView.h"
#include <array>

namespace Sailor::RHI
{
	struct RHISceneViewSnapshot;
	struct RHIVisibleSceneProxy;

	// An immutable view retained by MotionBlurNode, shared by its MRT producers.
	struct RHIMotionHistoryFrame
	{
		UboFrameData m_frameData{};
		WorldPtr m_world{};
		ObjectPtr m_cameraOwner{};
		uint64_t m_cameraRevision = 0ull;
		ESceneViewRenderMode m_renderMode = ESceneViewRenderMode::Lit;
		TSharedPtr<const TVector<RHISceneVersionPtr>> m_sceneVersions{};
		TSharedPtr<const TVector<glm::mat4>> m_bones{};
		std::array<uint64_t, 3> m_mobilityRevisions{};
	};

	struct RHIObjectMotionData
	{
		glm::mat4 m_previousModel{ 1.0f };
		// Previous skeleton offset, valid history, alpha blending, reserved.
		glm::uvec4 m_state{ 0xFFFFFFFFu, 0u, 0u, 0u };
		bool operator==(const RHIObjectMotionData&) const = default;
	};
	SAILOR_API RHIObjectMotionData MakeObjectMotionData(
		const glm::mat4& current, const glm::mat4& previous,
		uint32_t previousSkeletonOffset, bool valid);

	SAILOR_API RHIMotionHistoryFrame CaptureMotionHistory(
		const RHISceneViewSnapshot& snapshot, const glm::ivec2& extent);
	SAILOR_API void UploadMotionData(RHICommandListPtr commandList,
		const RHISceneViewSnapshot& snapshot, const glm::ivec2& extent);
	SAILOR_API bool IsMotionHistoryContinuous(
		const RHIMotionHistoryFrame& previous, const RHIMotionHistoryFrame& current);
	SAILOR_API bool ResolvePreviousMotionProxy(
		const RHISceneViewSnapshot& snapshot, const RHIVisibleSceneProxy& current,
		RHIVisibleSceneProxy& previous);
}
