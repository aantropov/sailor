#include "RHI/GpuFrameTimeQueryRing.h"

#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace Sailor;

namespace
{
	const char* g_currentTestName = "startup";

	[[noreturn]] void ReportTermination() noexcept
	{
		std::cerr << "GpuFrameTimeQueryTests terminated while running: " <<
			g_currentTestName << std::endl;
		std::_Exit(2);
	}

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	bool IsNear(float left, float right, float epsilon = 0.0001f)
	{
		return std::abs(left - right) <= epsilon;
	}

	template<typename TCallable>
	void RunTest(const char* name, TCallable&& callable)
	{
		g_currentTestName = name;
		callable();
	}

	void TestGpuFrameTimeQueryRingDoesNotReuseDelayedSlots()
	{
		float milliseconds = 0.0f;
		Require(
			RHI::TryResolveGpuFrameTimeMilliseconds(
				100u, 150u, 64u, 1000000.0f, milliseconds) &&
			milliseconds == 50.0f,
			"ordered timestamp results should resolve to milliseconds");
		Require(
			!RHI::TryResolveGpuFrameTimeMilliseconds(
				200u, 100u, 64u, 1.0f, milliseconds),
			"a stale full-width end timestamp must not become an enormous unsigned duration");
		Require(
			RHI::TryResolveGpuFrameTimeMilliseconds(
				0xfffffff0u, 0x10u, 32u, 1.0f, milliseconds),
			"limited-width timestamp counters should still support a valid wrap");
		Require(
			RHI::CalculateGpuFramesPerSecond(40.0f) == 25u &&
			RHI::CalculateGpuFramesPerSecond(0.0f) == 0u &&
			RHI::CalculateGpuFramesPerSecond(
				(std::numeric_limits<float>::quiet_NaN)()) == 0u,
			"GPU FPS must be derived from the measured GPU frame duration");

		RHI::TGpuFrameTimeQueryRing<2u> ring;
		const uint32_t first = ring.Acquire();
		Require(first == 0u && ring.MarkIssued(first),
			"first query slot should transition from recording to issued");
		const uint32_t second = ring.Acquire();
		Require(second == 1u && ring.MarkIssued(second),
			"second query slot should transition from recording to issued");
		Require(
			ring.Acquire() == RHI::TGpuFrameTimeQueryRing<2u>::InvalidSlot,
			"a delayed query ring must not reset or reissue slots that are still pending");
		Require(
			ring.GetState(first) == RHI::EGpuFrameTimeQuerySlotState::Issued &&
			ring.GetState(second) == RHI::EGpuFrameTimeQuerySlotState::Issued,
			"pending slots should retain completion ownership");
		Require(ring.MarkCompleted(second),
			"a ready query result should release exactly its owning slot");
		const uint32_t reused = ring.Acquire();
		Require(reused == second,
			"only the explicitly completed slot should become reusable");
		Require(ring.CancelRecording(reused),
			"a boundary command that failed before submission should be cancellable");
		Require(
			ring.GetState(first) == RHI::EGpuFrameTimeQuerySlotState::Issued,
			"cancelling another recording must not release a delayed issued query");
	}

	void TestGpuTimingAverageUsesRollingWindow()
	{
		RHI::TGpuTimingAverage<3u> average;
		Require(average.GetSampleCount() == 0u && average.GetAverage() == 0.0f,
			"an empty GPU timing average should start at zero");
		Require(!average.AddSample(-1.0f) &&
			!average.AddSample((std::numeric_limits<float>::quiet_NaN)()),
			"invalid GPU timings should not enter the average");

		Require(average.AddSample(1.0f) &&
			average.AddSample(2.0f) &&
			average.AddSample(6.0f) &&
			average.GetSampleCount() == 3u &&
			IsNear(average.GetAverage(), 3.0f),
			"GPU timing average should include all samples while warming up");
		Require(average.AddSample(10.0f) &&
			average.GetSampleCount() == 3u &&
			IsNear(average.GetAverage(), 6.0f),
			"GPU timing average should evict the oldest sample at capacity");
	}
}

int main()
{
	std::set_terminate(ReportTermination);

	try
	{
		RunTest("GpuFrameTimeQueryRingDoesNotReuseDelayedSlots",
			TestGpuFrameTimeQueryRingDoesNotReuseDelayedSlots);
		RunTest("GpuTimingAverageUsesRollingWindow",
			TestGpuTimingAverageUsesRollingWindow);
	}
	catch (const std::exception& exception)
	{
		std::cerr << "GpuFrameTimeQueryTests failed: " << exception.what() << std::endl;
		return 1;
	}

	std::cout << "GpuFrameTimeQueryTests passed." << std::endl;
	return 0;
}
