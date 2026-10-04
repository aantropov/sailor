#pragma once

#include "RHI/Types.h"
#include "RHI/GpuFrameTimeQueryRing.h"

namespace Sailor::RHI
{
	// Scope durations are rolling averages; age belongs to the newest included query.
	struct GpuTimingSnapshot : GpuTimingResult
	{
		float GetAgeMilliseconds(std::chrono::steady_clock::time_point now) const
		{
			return m_queryId != 0u ?
				std::chrono::duration<float, std::milli>(now - m_recordedAt).count() : 0.0f;
		}
	};

	// Render-thread state. Renderer publishes the result under its existing SpinLock.
	class RendererTimings final
	{
	public:
		using Clock = std::chrono::steady_clock;

		bool RecordFrame(Clock::time_point now, const FrameSubmissionResult& submission)
		{
			if (!submission.m_bSubmitted)
			{
				return false;
			}
			if (!m_bHasFrame)
			{
				m_bHasFrame = true;
				m_intervalStartedAt = now;
				return false;
			}

			++m_renderFrames;
			m_presentedFrames += submission.m_bPresented ? 1u : 0u;
			const double seconds = std::chrono::duration<double>(now - m_intervalStartedAt).count();
			if (seconds < 1.0)
			{
				return false;
			}

			m_renderFps = static_cast<uint32_t>(m_renderFrames / seconds + 0.5);
			m_presentFps = static_cast<uint32_t>(m_presentedFrames / seconds + 0.5);
			m_renderFrames = 0u;
			m_presentedFrames = 0u;
			m_intervalStartedAt = now;
			return true;
		}

		void ResetFrameCadence()
		{
			m_bHasFrame = false;
			m_renderFrames = 0u;
			m_presentedFrames = 0u;
			m_renderFps = 0u;
			m_presentFps = 0u;
		}

		uint32_t GetRenderFps() const { return m_renderFps; }
		uint32_t GetPresentFps() const { return m_presentFps; }

		void ResetGpuTimings(uint64_t generation)
		{
			m_history.Clear();
			m_gpuTimings = {};
			m_gpuTimings.m_generation = generation;
		}

		bool PublishGpuTimings(const std::optional<GpuTimingResult>& pendingResult)
		{
			if (!pendingResult)
			{
				return false;
			}
			const auto& result = *pendingResult;
			if (result.m_generation != m_gpuTimings.m_generation ||
				result.m_queryId <= m_gpuTimings.m_queryId)
			{
				return false;
			}

			m_gpuTimings.m_queryId = result.m_queryId;
			m_gpuTimings.m_recordedAt = result.m_recordedAt;
			m_gpuTimings.m_bValid = result.m_bValid;
			m_gpuTimings.m_gpuWorkMilliseconds = result.m_bValid ? result.m_gpuWorkMilliseconds : 0.0f;
			m_gpuTimings.m_timings.Clear();
			if (!result.m_bValid)
			{
				m_history.Clear();
				return true;
			}

			TVector<GpuTiming> frameTimings;
			frameTimings.Reserve(result.m_timings.Num());
			for (const auto& timing : result.m_timings)
			{
				const size_t existing = frameTimings.FindIf([&timing](const GpuTiming& value)
					{
						return value.m_name == timing.m_name && value.m_queue == timing.m_queue;
					});
				if (existing == TVector<GpuTiming>::InvalidIndex)
				{
					frameTimings.Add(timing);
				}
				else
				{
					frameTimings[existing].m_durationMilliseconds += timing.m_durationMilliseconds;
				}
			}

			for (const auto& timing : frameTimings)
			{
				size_t history = m_history.FindIf([&timing](const TimingHistory& value)
					{
						return value.m_name == timing.m_name && value.m_queue == timing.m_queue;
					});
				if (history == TVector<TimingHistory>::InvalidIndex)
				{
					history = m_history.Emplace();
					m_history[history].m_name = timing.m_name;
					m_history[history].m_queue = timing.m_queue;
				}
				m_history[history].m_average.AddSample(timing.m_durationMilliseconds);
				m_history[history].m_lastQueryId = result.m_queryId;
			}

			m_history.RemoveAll([&result](const TimingHistory& history)
				{
					return history.m_lastQueryId != result.m_queryId;
				});
			m_gpuTimings.m_timings.Reserve(m_history.Num());
			for (const auto& history : m_history)
			{
				m_gpuTimings.m_timings.Add({ history.m_name, history.m_queue, history.m_average.GetAverage() });
			}
			m_gpuTimings.m_timings.Sort([](const GpuTiming& lhs, const GpuTiming& rhs)
				{
					return lhs.m_durationMilliseconds > rhs.m_durationMilliseconds;
				});
			return true;
		}

		const GpuTimingSnapshot& GetGpuTimings() const { return m_gpuTimings; }

	private:
		struct TimingHistory
		{
			std::string m_name;
			ECommandListQueue m_queue = ECommandListQueue::Graphics;
			TGpuTimingAverage<60u> m_average;
			uint64_t m_lastQueryId = 0u;
		};

		Clock::time_point m_intervalStartedAt{};
		bool m_bHasFrame = false;
		uint32_t m_renderFrames = 0u;
		uint32_t m_presentedFrames = 0u;
		uint32_t m_renderFps = 0u;
		uint32_t m_presentFps = 0u;
		TVector<TimingHistory> m_history;
		GpuTimingSnapshot m_gpuTimings;
	};
}
