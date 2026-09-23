#include "RHI/RendererTimings.h"
#include "GraphicsDriver/Vulkan/VulkanGraphicsDriver.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace std::chrono_literals;

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	class GpuTimingDriverProbe final : public GraphicsDriver::Vulkan::VulkanGraphicsDriver
	{
	public:
		// Seed recorded query inputs without a Vulkan device; End/Cancel/Take are the real driver methods.
		uint32_t SeedRecording(uint64_t generation, uint64_t queryId, RendererTimings::Clock::time_point recordedAt)
		{
			const uint32_t slot = m_gpuFrameTimeQuerySlots.Acquire();
			Require(slot != TGpuFrameTimeQueryRing<NumGpuFrameTimeQuerySlots>::InvalidSlot,
				"the query fixture must have a free recording slot");
			m_gpuTimingQueries[slot] = { generation, queryId, recordedAt };
			m_activeGpuFrameTimeQuerySlot = slot;
			return slot;
		}

		void SeedRange(uint32_t slot, bool bEnded)
		{
			m_gpuFrameTimeRangeCounts[slot] = 1u;
			m_gpuFrameTimeRanges[slot][0].m_bEnded = bEnded;
		}

		void SeedScope(uint32_t slot, bool bEnded)
		{
			m_gpuTimingScopeCounts[slot] = 1u;
			m_gpuTimingScopeRecords[slot][0].m_bEnded = bEnded;
		}

		EGpuFrameTimeQuerySlotState GetQueryState(uint32_t slot) const
		{
			return m_gpuFrameTimeQuerySlots.GetState(slot);
		}
	};

	bool IsNear(float left, float right)
	{
		return std::abs(left - right) < 0.0001f;
	}

	GpuTimingResult Result(uint64_t generation, uint64_t queryId,
		RendererTimings::Clock::time_point recordedAt, float gpuWorkMilliseconds,
		std::initializer_list<GpuTiming> timings)
	{
		GpuTimingResult result;
		result.m_generation = generation;
		result.m_queryId = queryId;
		result.m_recordedAt = recordedAt;
		result.m_bValid = true;
		result.m_gpuWorkMilliseconds = gpuWorkMilliseconds;
		result.m_timings = timings;
		return result;
	}

	const GpuTiming& FindTiming(const GpuTimingSnapshot& snapshot, const char* name, ECommandListQueue queue)
	{
		const size_t index = snapshot.m_timings.FindIf([=](const GpuTiming& timing)
			{
				return timing.m_name == name && timing.m_queue == queue;
			});
		Require(index != TVector<GpuTiming>::InvalidIndex, "published snapshot must retain each name/queue identity");
		return snapshot.m_timings[index];
	}

	void TestWallClockCadenceDoesNotDependOnGpuDuration()
	{
		RendererTimings queriesOn;
		RendererTimings queriesOff;
		queriesOn.ResetGpuTimings(1);
		const RendererTimings::Clock::time_point start{};
		Require(!queriesOn.RecordFrame(start, { true, true }) && !queriesOff.RecordFrame(start, { true, true }),
			"the first completed frame primes the cadence interval");
		for (uint32_t frame = 1; frame <= 10; ++frame)
		{
			const auto now = start + frame * 100ms;
			// CPU preparation, acquire waits and idle time fill the rest of each 100 ms interval.
			queriesOn.PublishGpuTimings(Result(1, frame, now - 5ms, 5.0f,
				{ { "Render", ECommandListQueue::Graphics, 5.0f } }));
			const bool updated = queriesOn.RecordFrame(now, { true, true });
			Require(updated == queriesOff.RecordFrame(now, { true, true }) && updated == (frame == 10),
				"query readiness must not change the wall-clock reporting interval");
		}
		Require(queriesOn.GetRenderFps() == 10 && queriesOn.GetPresentFps() == 10 &&
			queriesOff.GetRenderFps() == 10 && queriesOff.GetPresentFps() == 10,
			"5 ms GPU work at 100 ms frame intervals is 10 FPS, not 200 FPS");
		queriesOn.ResetGpuTimings(2);
		Require(queriesOn.GetRenderFps() == 10, "disabling or restarting profiling must not reset frame cadence");
		Require(!queriesOn.RecordFrame(start + 1500ms, { true, true }) &&
			queriesOn.RecordFrame(start + 2500ms, { true, true }),
			"long gaps between render tasks must stay in the cadence denominator");
		Require(queriesOn.GetRenderFps() == 1 && queriesOn.GetPresentFps() == 1,
			"two completed frames over 1.5 seconds must not be reported as two FPS");
	}

	void TestOffscreenFramesAreNotPresents()
	{
		RendererTimings timings;
		const RendererTimings::Clock::time_point start{};
		timings.RecordFrame(start, { true, false });
		for (uint32_t frame = 1; frame <= 20; ++frame)
		{
			timings.RecordFrame(start + frame * 50ms, { true, false });
		}
		Require(timings.GetRenderFps() == 20 && timings.GetPresentFps() == 0,
			"offscreen render submissions must not manufacture swapchain presents");
		for (uint32_t frame = 1; frame <= 20; ++frame)
		{
			timings.RecordFrame(start + 1s + frame * 50ms, { true, frame % 2 == 0 });
		}
		Require(timings.GetRenderFps() == 20 && timings.GetPresentFps() == 10,
			"mixed offscreen and swapchain frames need separate cadence counters");
		timings.ResetFrameCadence();
		Require(timings.GetRenderFps() == 0 && timings.GetPresentFps() == 0,
			"failed frame submission must clear the reported cadence");
		Require(!timings.RecordFrame(start + 10s, { true, true }), "the next successful frame must start a fresh interval");
	}

	void TestAcceptedSubmitSurvivesFailedPresent()
	{
		RendererTimings timings;
		const RendererTimings::Clock::time_point start{};
		timings.RecordFrame(start, { true, true });
		for (uint32_t frame = 1; frame <= 10; ++frame)
		{
			Require(!timings.RecordFrame(start + frame * 100ms - 50ms, {}),
				"a rejected submission must not contribute a rendered or presented frame");
			// An out-of-date swapchain can reject presentation after accepting its GPU submission.
			const FrameSubmissionResult submission{ true, frame != 10 };
			timings.RecordFrame(start + frame * 100ms, submission);
		}
		Require(timings.GetRenderFps() == 10 && timings.GetPresentFps() == 9,
			"a successful submit with failed present still contributes to render cadence");
	}

	void TestGpuAggregationPreservesQueues()
	{
		RendererTimings timings;
		timings.ResetGpuTimings(7);
		const RendererTimings::Clock::time_point start{};
		Require(timings.PublishGpuTimings(Result(7, 1, start, 12.0f,
			{
				{ "Lighting", ECommandListQueue::Graphics, 1.0f },
				{ "Lighting", ECommandListQueue::Graphics, 2.0f },
				{ "Lighting", ECommandListQueue::Compute, 4.0f },
				{ "Copy", ECommandListQueue::Transfer, 2.0f }
			})), "a ready result must publish a snapshot");
		const auto first = timings.GetGpuTimings();
		Require(first.m_bValid && first.m_timings.Num() == 3 && first.m_gpuWorkMilliseconds == 12.0f,
			"GPU work and per-name/queue timings must remain distinct values");
		Require(IsNear(FindTiming(first, "Lighting", ECommandListQueue::Graphics).m_durationMilliseconds, 3.0f) &&
			IsNear(FindTiming(first, "Lighting", ECommandListQueue::Compute).m_durationMilliseconds, 4.0f),
			"sum repeated scopes within one queue, never across queues");
		Require(first.m_timings[0].m_queue == ECommandListQueue::Compute,
			"sorting slowest timings must preserve queue metadata");

		timings.PublishGpuTimings(Result(7, 2, start + 10ms, 20.0f,
			{
				{ "Lighting", ECommandListQueue::Graphics, 10.0f },
				{ "Lighting", ECommandListQueue::Compute, 8.0f }
			}));
		const auto& second = timings.GetGpuTimings();
		Require(second.m_timings.Num() == 2 &&
			IsNear(FindTiming(second, "Lighting", ECommandListQueue::Graphics).m_durationMilliseconds, 6.5f) &&
			IsNear(FindTiming(second, "Lighting", ECommandListQueue::Compute).m_durationMilliseconds, 6.0f),
			"rolling averages must be independent for each queue and drop absent scopes");
	}

	void TestPendingInvalidAndNewGeneration()
	{
		RendererTimings timings;
		timings.ResetGpuTimings(10);
		const RendererTimings::Clock::time_point start{};
		Require(!timings.PublishGpuTimings(std::nullopt) && !timings.GetGpuTimings().m_bValid &&
			timings.GetGpuTimings().m_queryId == 0, "pending before the first sample is not a valid zero-duration result");
		timings.PublishGpuTimings(Result(10, 1, start, 4.0f,
			{ { "Compute only", ECommandListQueue::Compute, 4.0f } }));
		Require(!timings.PublishGpuTimings(std::nullopt), "an asynchronous pending poll must not replace the last result");
		const auto& pending = timings.GetGpuTimings();
		Require(pending.m_bValid && pending.m_queryId == 1 && pending.m_timings[0].m_queue == ECommandListQueue::Compute &&
			IsNear(pending.GetAgeMilliseconds(start + 250ms), 250.0f),
			"retained timings must expose their real age and retain Compute metadata");

		auto invalid = Result(10, 2, start + 300ms, 0.0f, {});
		invalid.m_bValid = false;
		Require(timings.PublishGpuTimings(invalid), "an invalid completed query is not a pending poll");
		Require(!timings.GetGpuTimings().m_bValid && timings.GetGpuTimings().m_queryId == 2 &&
			timings.GetGpuTimings().m_timings.IsEmpty(), "invalid results must clear the previous timings");
		timings.PublishGpuTimings(Result(10, 3, start + 400ms, 8.0f,
			{ { "Compute only", ECommandListQueue::Compute, 8.0f } }));
		Require(IsNear(timings.GetGpuTimings().m_timings[0].m_durationMilliseconds, 8.0f),
			"query failure must not leave old samples in the next rolling average");

		// The renderer uses this reset for disabled queries, graph changes and submission failure.
		timings.ResetGpuTimings(11);
		Require(!timings.GetGpuTimings().m_bValid && timings.GetGpuTimings().m_queryId == 0 &&
			timings.GetGpuTimings().m_timings.IsEmpty(), "a new profiling generation must invalidate the old snapshot");
		Require(!timings.PublishGpuTimings(Result(10, 4, start + 450ms, 99.0f,
			{ { "Old graph", ECommandListQueue::Graphics, 99.0f } })),
			"a delayed query from the old graph must not repopulate the new generation");
		Require(timings.PublishGpuTimings(Result(11, 5, start + 500ms, 1.0f, {})) &&
			timings.GetGpuTimings().m_bValid && timings.GetGpuTimings().m_timings.IsEmpty(),
			"a valid GPU-work result without named scopes is distinct from pending and invalid");
	}

	void TestEmptyQueryClearsSnapshotInSameFrame()
	{
		GpuTimingDriverProbe driver;
		RendererTimings timings;
		timings.ResetGpuTimings(3);
		const RendererTimings::Clock::time_point start{};
		timings.PublishGpuTimings(Result(3, 1, start, 4.0f,
			{ { "Render", ECommandListQueue::Graphics, 4.0f } }));

		const uint32_t emptySlot = driver.SeedRecording(3, 2, start + 100ms);
		driver.EndGpuFrameTimeQuery();
		const auto emptyResult = driver.TakeGpuTimingResult();
		Require(emptyResult && !emptyResult->m_bValid && emptyResult->m_generation == 3 &&
			emptyResult->m_queryId == 2 && emptyResult->m_recordedAt == start + 100ms &&
			emptyResult->m_gpuWorkMilliseconds == 0.0f && emptyResult->m_timings.IsEmpty(),
			"ending a zero-range query must publish explicit invalid metadata, not pending");
		Require(driver.GetQueryState(emptySlot) == EGpuFrameTimeQuerySlotState::Available,
			"ending an empty query must release its recording slot");
		Require(timings.PublishGpuTimings(emptyResult) && !timings.GetGpuTimings().m_bValid &&
			timings.GetGpuTimings().m_gpuWorkMilliseconds == 0.0f && timings.GetGpuTimings().m_timings.IsEmpty(),
			"consuming End's result in the same frame must remove the previous measured snapshot");
		Require(!timings.PublishGpuTimings(driver.TakeGpuTimingResult()) &&
			!timings.GetGpuTimings().m_bValid && timings.GetGpuTimings().m_queryId == 2,
			"taking an already consumed invalid result must not resurrect previous measurements");

		timings.PublishGpuTimings(Result(3, 3, start + 200ms, 8.0f,
			{ { "Render", ECommandListQueue::Graphics, 8.0f } }));
		Require(timings.GetGpuTimings().m_bValid && timings.GetGpuTimings().m_queryId == 3 &&
			IsNear(timings.GetGpuTimings().m_timings[0].m_durationMilliseconds, 8.0f),
			"measurement after an empty query must start a fresh rolling average");
	}

	void TestIncompleteAndCancelledQueriesAreInvalid()
	{
		GpuTimingDriverProbe driver;
		const RendererTimings::Clock::time_point start{};
		const uint32_t rangeSlot = driver.SeedRecording(4, 1, start);
		driver.SeedRange(rangeSlot, false);
		driver.EndGpuFrameTimeQuery();
		const auto unfinishedRange = driver.TakeGpuTimingResult();
		Require(unfinishedRange && !unfinishedRange->m_bValid && unfinishedRange->m_queryId == 1 &&
			driver.GetQueryState(rangeSlot) == EGpuFrameTimeQuerySlotState::Available,
			"an unfinished GPU range must cancel with an explicit invalid result");

		const uint32_t scopeSlot = driver.SeedRecording(4, 2, start + 100ms);
		driver.SeedRange(scopeSlot, true);
		driver.SeedScope(scopeSlot, false);
		driver.EndGpuFrameTimeQuery();
		const auto unfinishedScope = driver.TakeGpuTimingResult();
		Require(unfinishedScope && !unfinishedScope->m_bValid && unfinishedScope->m_generation == 4 &&
			unfinishedScope->m_queryId == 2 && unfinishedScope->m_recordedAt == start + 100ms &&
			driver.GetQueryState(scopeSlot) == EGpuFrameTimeQuerySlotState::Available,
			"an unfinished named scope must publish the cancelled query's metadata");

		const uint32_t pendingSlot = driver.SeedRecording(4, 3, start + 200ms);
		driver.SeedRange(pendingSlot, true);
		driver.SeedScope(pendingSlot, true);
		driver.EndGpuFrameTimeQuery();
		Require(!driver.TakeGpuTimingResult() &&
			driver.GetQueryState(pendingSlot) == EGpuFrameTimeQuerySlotState::Recording,
			"a complete recording remains pending until the frame is submitted or cancelled");
		const FrameSubmissionResult rejected = driver.SubmitFrameWithoutPresent({}, {});
		Require(!rejected.m_bSubmitted && !rejected.m_bPresented,
			"a driver without a device must reject both submission and presentation");
		const auto cancelled = driver.TakeGpuTimingResult();
		Require(cancelled && !cancelled->m_bValid && cancelled->m_generation == 4 &&
			cancelled->m_queryId == 3 && cancelled->m_recordedAt == start + 200ms &&
			driver.GetQueryState(pendingSlot) == EGpuFrameTimeQuerySlotState::Available,
			"a failed submission must publish the pending query as invalid and release its slot");
		driver.CancelGpuFrameTimeQuery();
		Require(!driver.TakeGpuTimingResult(), "cancelling without a recording must not publish duplicate invalid results");
	}

	void TestCancelledQueryOrdering()
	{
		GpuTimingDriverProbe driver;
		RendererTimings timings;
		timings.ResetGpuTimings(5);
		const RendererTimings::Clock::time_point start{};
		driver.SeedRecording(5, 20, start + 20ms);
		driver.EndGpuFrameTimeQuery();
		driver.SeedRecording(5, 19, start + 19ms);
		driver.EndGpuFrameTimeQuery();
		const auto newest = driver.TakeGpuTimingResult();
		Require(newest && !newest->m_bValid && newest->m_queryId == 20,
			"the driver must not overwrite a newer cancellation with an older slot result");
		timings.PublishGpuTimings(newest);
		Require(!timings.PublishGpuTimings(Result(5, 19, start + 19ms, 9.0f,
			{ { "Render", ECommandListQueue::Graphics, 9.0f } })) &&
			!timings.GetGpuTimings().m_bValid && timings.GetGpuTimings().m_queryId == 20,
			"a delayed older measurement must not revive the snapshot after a newer cancellation");

		timings.ResetGpuTimings(6);
		Require(!timings.PublishGpuTimings(newest) && timings.GetGpuTimings().m_queryId == 0,
			"a cancelled query from the old graph must not change the new generation");
	}

	void TestOutOfOrderAndRollingWindow()
	{
		RendererTimings timings;
		timings.ResetGpuTimings(1);
		const RendererTimings::Clock::time_point start{};
		auto latest = Result(1, 20, start + 20ms, 2.0f,
			{ { "Node", ECommandListQueue::Compute, 2.0f } });
		timings.PublishGpuTimings(latest);
		Require(!timings.PublishGpuTimings(latest), "polling the same completed query twice must not add a second sample");
		auto older = Result(1, 19, start + 19ms, 100.0f, {});
		older.m_bValid = false;
		Require(!timings.PublishGpuTimings(older) && timings.GetGpuTimings().m_bValid &&
			timings.GetGpuTimings().m_queryId == 20, "an older ready slot must not overwrite or invalidate a newer result");

		timings.ResetGpuTimings(2);
		for (uint64_t query = 1; query <= 61; ++query)
		{
			timings.PublishGpuTimings(Result(2, query, start + query * 1ms, static_cast<float>(query),
				{ { "Node", ECommandListQueue::Compute, static_cast<float>(query) } }));
		}
		Require(IsNear(timings.GetGpuTimings().m_timings[0].m_durationMilliseconds, 31.5f),
			"the production timing history must retain its most recent 60 samples");
	}
}

int main()
{
	try
	{
		TestWallClockCadenceDoesNotDependOnGpuDuration();
		TestOffscreenFramesAreNotPresents();
		TestAcceptedSubmitSurvivesFailedPresent();
		TestGpuAggregationPreservesQueues();
		TestPendingInvalidAndNewGeneration();
		TestEmptyQueryClearsSnapshotInSameFrame();
		TestIncompleteAndCancelledQueriesAreInvalid();
		TestCancelledQueryOrdering();
		TestOutOfOrderAndRollingWindow();
		std::cout << "RendererTimingTests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "RendererTimingTests failed: " << error.what() << '\n';
		return 1;
	}
}
