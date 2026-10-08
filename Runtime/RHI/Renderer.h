#pragma once
#include <functional>
#include <atomic>
#include <thread>

#include "Core/Defines.h"
#include "Core/SpinLock.h"
#include "Math/Math.h"
#include "Engine/Types.h"
#include "RHI/Types.h"
#include "Containers/ConcurrentMap.h"
#include "Memory/RefPtr.hpp"
#include "Memory/SharedPtr.hpp"
#include "Memory/UniquePtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "Core/Submodule.h"
#include "Tasks/Scheduler.h"
#include "GraphicsDriver.h"
#include "RendererTimings.h"
#include "Readback.h"
#include "SceneView.h"
#include "FrameGraph/SkyParameters.h"

namespace Sailor::Framegraph { class SkyNode; }

namespace Sailor
{
	class FrameState;
}

namespace Sailor::RHI
{
	class Renderer : public TSubmodule<Renderer>
	{
	public:

		// TODO: Move to RHI::Constants?
		static constexpr uint32_t GPUCullingGroupSize = 256;

			SAILOR_API Renderer(Platform::Window* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug);
			SAILOR_API ~Renderer() override;
			SAILOR_API bool IsInitialized() const { return m_bIsInitialized; }

		SAILOR_API RHI::EMsaaSamples GetMsaaSamples() const { return m_msaaSamples; }
		SAILOR_API RHI::EFormat GetColorFormat() const;
		SAILOR_API RHI::EFormat GetDepthFormat() const;

		SAILOR_API void FixLostDevice();
		SAILOR_API bool PushFrame(const Sailor::FrameState& frame);
		SAILOR_API void WaitIdle();

		SAILOR_API const Stats& GetStats() const { return m_stats; }
		SAILOR_API GpuTimingSnapshot GetGpuTimings() const;
		SAILOR_API void RefreshGpuTimings() { m_gpuTimingGeneration.fetch_add(1u, std::memory_order_release); }
		SAILOR_API RHIGlobalIlluminationRenderStats
			GetGlobalIlluminationRenderStats() const;

		SAILOR_API static TUniquePtr<IGraphicsDriver>& GetDriver();
		SAILOR_API static IGraphicsDriverCommands* GetDriverCommands();

		SAILOR_API RHISceneViewPtr GetOrAddSceneView(WorldPtr worldPtr);
		SAILOR_API void RemoveSceneView(WorldPtr worldPtr);

		SAILOR_API bool BeginConditionalDestroy();
		SAILOR_API void RefreshFrameGraph() { m_bFrameGraphOutdated = true; }
		SAILOR_API bool EnsureFrameGraph();

		SAILOR_API FrameGraphPtr GetFrameGraph() { return m_frameGraph; }
		// Render queues completed captures; only Main reads the published frame.
		SAILOR_API void QueueEditorReadback(ReadbackFramePtr frame);
		SAILOR_API ReadbackFramePtr GetEditorReadback() const { return m_editorReadback; }
		SAILOR_API bool HasEditorReadback() const { return m_bHasEditorReadback; }

		SAILOR_API static void MemoryStats();

	private:
		friend class RendererSubmissionTestAccess;
		struct FrameSubmission;
		void CaptureSceneView(FrameSubmission& submission, const Sailor::FrameState& frame);
		bool AcquireSubmission(FrameSubmission& submission);
		void PrepareSceneView(FrameSubmission& submission);
		void RecordAndSubmitFrame(FrameSubmission& submission, const Sailor::FrameState& frame);
		void CompleteFrame(FrameSubmission& submission);
		void ReturnSceneView(RHISceneViewPtr& sceneView);

	protected:
		SAILOR_API void UpdateSkyParameters(WorldPtr world, RHIFrameGraphPtr graph);
		void UpdateMemoryStats();
		void PublishGpuTimings(const std::optional<GpuTimingResult>& timings);
		void InvalidateGpuTimings();
		void ResetFrameCadence();
		void UpdateGlobalIlluminationRenderStats(
			const RHIGlobalIlluminationRenderStats& stats);

		RHI::EMsaaSamples m_msaaSamples;

		std::atomic<bool> m_bFrameGraphOutdated = false;
		std::atomic<bool> m_bForceStop = false;
		std::atomic<bool> m_bIsFrameQueued = false;

		RHI::Stats m_stats{};

		mutable SpinLock m_globalIlluminationStatsLock;
		RHIGlobalIlluminationRenderStats m_globalIlluminationStats{};

		mutable SpinLock m_gpuTimingsLock;
		RendererTimings m_timings;
		GpuTimingSnapshot m_gpuTimings;
		std::atomic<uint64_t> m_gpuTimingGeneration = 0u;
		uint64_t m_profiledFrameGraphGeneration = 0u;
		bool m_bGpuQueriesEnabled = false;

		Platform::Window* m_pViewport;

		FrameGraphPtr m_frameGraph{};
		TRefPtr<Framegraph::SkyNode> m_skyNode;
		SkyParameters m_publishedSkyParams;
		ReadbackFramePtr m_editorReadback{};
		bool m_bHasEditorReadback = false;
		TConcurrentMap<WorldPtr, TList<TPair<RHISceneViewPtr,bool>>, 4, ERehashPolicy::Never> m_cachedSceneViews{};
			TUniquePtr<IGraphicsDriver> m_driverInstance{};
			Tasks::ITaskPtr m_previousRenderFrame{};
			Tasks::ITaskPtr m_previousSceneVersionRelease{};
			TVector<RHIRenderSubmissionContextPtr> m_submissionContexts{};
			std::atomic<uint64_t> m_nextSubmissionId = 1ull;
			uint64_t m_frameGraphResourceGeneration = 0ull;
			bool m_bIsInitialized = false;
		};
	};
