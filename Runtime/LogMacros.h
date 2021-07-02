#pragma once
#include <mutex>
#include <string>

extern std::mutex m_logMutex;

#define SAILOR_LOG(Format, ...) \
	{ \
	const std::lock_guard<std::mutex> lock(m_logMutex); \
	printf(Format, __VA_ARGS__); \
	printf("\n"); \
	}