#include "Time.h"

#include <chrono>

#if defined(_WIN32)
#include "Sailor.h"
#include "Tasks/Tasks.h"
#include "Tasks/Scheduler.h"
#endif

using namespace Sailor;
using namespace Sailor::Utils;

int64_t Utils::GetCurrentTimeMs()
{
	return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t Utils::GetCurrentTimeMicro()
{
	return (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t Utils::GetCurrentTimeNano()
{
	return (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

#if !defined(_WIN32)
namespace
{
	int64_t GetElapsedTimeMicro()
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}
}
#endif

void Utils::Timer::Start()
{
#if defined(_WIN32)
	LARGE_INTEGER li;
	if (!QueryPerformanceFrequency(&li))
	{
		SAILOR_LOG("QueryPerformanceFrequency failed!");
	}

	m_pcFrequence = double(li.QuadPart) / 1000.0;

	QueryPerformanceCounter(&li);
	m_counterStart = li.QuadPart;
#else
	m_pcFrequence = 1000.0;
	m_counterStart = GetElapsedTimeMicro();
#endif

	m_bIsStarted = true;
}

void Utils::Timer::Stop()
{
	if (!m_bIsStarted)
	{
		return;
	}

#if defined(_WIN32)
	LARGE_INTEGER li;
	QueryPerformanceCounter(&li);
	m_counterEnd = li.QuadPart;
#else
	m_counterEnd = GetElapsedTimeMicro();
#endif

	m_counterAcc += m_counterEnd - m_counterStart;

	m_bIsStarted = false;
}

int64_t Utils::Timer::ResultMs() const
{
	if (m_pcFrequence == 0.0)
	{
		return 0;
	}

	if (m_bIsStarted)
	{
#if defined(_WIN32)
		LARGE_INTEGER li;
		QueryPerformanceCounter(&li);
		return int64_t(double(li.QuadPart - m_counterStart) / m_pcFrequence);
#else
		return int64_t((GetElapsedTimeMicro() - m_counterStart) / 1000);
#endif
	}
#if defined(_WIN32)
	return int64_t(double(m_counterEnd - m_counterStart) / m_pcFrequence);
#else
	return int64_t((m_counterEnd - m_counterStart) / 1000);
#endif
}

int64_t Utils::Timer::ResultAccumulatedMs() const
{
	if (m_pcFrequence == 0.0)
	{
		return 0;
	}

	if (m_bIsStarted)
	{
#if defined(_WIN32)
		LARGE_INTEGER li;
		QueryPerformanceCounter(&li);
		return int64_t(double(li.QuadPart - m_counterStart + m_counterAcc) / m_pcFrequence);
#else
		return int64_t((GetElapsedTimeMicro() - m_counterStart + m_counterAcc) / 1000);
#endif
	}

#if defined(_WIN32)
	return int64_t((double)m_counterAcc / m_pcFrequence);
#else
	return int64_t(m_counterAcc / 1000);
#endif
}

void Utils::Timer::Clear()
{
	m_counterStart = 0;
	m_counterEnd = 0;
	m_counterAcc = 0;
	m_pcFrequence = 0.0;
	m_bIsStarted = false;
}
