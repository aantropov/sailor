#pragma once

#include "Core/SpinLock.h"
#include "FrameGraph/LocalReflection.h"
#include "RHI/SceneView.h"

#include <array>
#include <optional>

namespace Sailor
{
	enum class ELocalReflectionCaptureState : uint8_t
	{
		Idle,
		Pending,
		Capturing,
		Filtering,
		Ready,
		Failed
	};

	struct LocalReflectionCaptureStatus
	{
		ELocalReflectionCaptureState m_state = ELocalReflectionCaptureState::Idle;
		uint64_t m_requestedGeneration = 0u;
		uint64_t m_publishedGeneration = 0u;
		uint64_t m_capturesSubmitted = 0u;
	};

	class LocalReflectionCaptureJob final : public RHI::RHIResource
	{
	public:
		Framegraph::LocalReflectionCaptureRequest m_request;
		uint64_t m_generation = 0u;
		uint32_t m_firstCamera = 0u;
		std::array<RHI::RHISceneViewPtr, 6> m_faces;
		RHI::RHIFrameGraphPtr m_graph;
		RHI::RHICubemapPtr m_cubemap;
		RHI::RHIFencePtr m_completion;
		RHI::RHISubmissionCompletionTokenPtr m_submission;
		RHI::RHIRenderSubmissionContextPtr m_context;
		bool m_bPrepared = false;
		bool m_bSubmitted = false;
		bool m_bRetry = false;
		std::atomic_bool m_bCancelled = false;

		static bool WarmupShaders();
		void AppendCameras(const RHI::RHISceneViewPtr& source);
		void ExtractSnapshots(const RHI::RHISceneViewPtr& source);
		TVector<Tasks::ITaskPtr> Prepare(const RHI::RHIFrameGraphPtr& source,
			const RHI::RHIRenderSubmissionContextPtr& context);
		bool Submit(RHI::RHISemaphorePtr& chainSemaphore);
	};

	// Owned by LightingECS. Main requests snapshots; Render records and publishes
	// them without retaining a World/ECS pointer across world destruction.
	class LocalReflectionCaptureState final
	{
	public:
		uint64_t Request(const Framegraph::LocalReflectionCaptureRequest& request);
		bool Reset();
		LocalReflectionCaptureStatus GetStatus() const;
		Framegraph::LocalReflectionCubemap GetCapture() const;
		TRefPtr<LocalReflectionCaptureJob> Capture(const RHI::RHISceneViewPtr& source);
		void Poll(const RHI::RHIFrameGraphPtr& graph);
		void Cancel(const TRefPtr<LocalReflectionCaptureJob>& job);

	private:
		mutable SpinLock m_lock;
		LocalReflectionCaptureStatus m_status;
		std::optional<Framegraph::LocalReflectionCaptureRequest> m_pending;
		TRefPtr<LocalReflectionCaptureJob> m_active;
		Framegraph::LocalReflectionCubemap m_capture;
		Tasks::TaskPtr<bool> m_warmup;
		uint64_t m_nextGeneration = 0u;
		uint64_t m_resetGeneration = 0u;
		bool m_bResetPending = false;
		std::atomic_bool m_bHasWork = false;
	};
}
