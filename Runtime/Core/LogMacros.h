#pragma once
#include "Core/Defines.h"
#include <cstdint>
#include <cstdio>

namespace Sailor
{
	enum class ELogSeverity : uint8_t
	{
		Info,
		Error
	};

#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 2, 3)))
#endif
	SAILOR_SHARED_API void LogMessage(ELogSeverity severity, const char* format, ...);
}

#if defined(_WIN32)
#define SAILOR_SNPRINTF sprintf_s
#else
#define SAILOR_SNPRINTF std::snprintf
#endif

#define SAILOR_LOG(Format, ...) ::Sailor::LogMessage(::Sailor::ELogSeverity::Info, Format __VA_OPT__(,) __VA_ARGS__)
#define SAILOR_LOG_ERROR(Format, ...) ::Sailor::LogMessage(::Sailor::ELogSeverity::Error, Format __VA_OPT__(,) __VA_ARGS__)
