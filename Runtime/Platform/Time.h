#pragma once

#include "Core/Defines.h"

#include <cstdint>
#include <span>

namespace Sailor::Utils
{
	SAILOR_API int64_t GetCurrentTimeMs();
	SAILOR_API int64_t GetCurrentTimeMicro();
	SAILOR_API int64_t GetCurrentTimeNano();

	struct FrameTimeStats
	{
		size_t m_numFrames = 0;
		double m_elapsedSeconds = 0.0;
		double m_sustainedFps = 0.0;
		float m_minMs = 0.0f, m_meanMs = 0.0f, m_maxMs = 0.0f;
		float m_p50Ms = 0.0f, m_p95Ms = 0.0f, m_p99Ms = 0.0f;
	};

	// Positive finite intervals; nearest-rank percentiles. The caller identifies
	// whether intervals describe simulation, presentation or GPU time.
	SAILOR_API FrameTimeStats CalculateFrameTimeStats(std::span<const float> seconds);

	struct SAILOR_API Timer
	{
		int64_t m_counterStart = 0;
		int64_t m_counterEnd = 0;
		int64_t m_counterAcc = 0;
		double m_pcFrequence = 0.0;
		bool m_bIsStarted = false;

		void Start();
		void Stop();

		int64_t ResultMs() const;
		int64_t ResultAccumulatedMs() const;

		void Clear();
	};
}
