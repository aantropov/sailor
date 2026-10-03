#pragma once

#include "Core/Defines.h"

#include <cstdint>

namespace Sailor::Utils
{
	SAILOR_API int64_t GetCurrentTimeMs();
	SAILOR_API int64_t GetCurrentTimeMicro();
	SAILOR_API int64_t GetCurrentTimeNano();

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
