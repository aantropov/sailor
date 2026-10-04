#pragma once

#include "Core/Defines.h"

#include <cstddef>
#include <string>
#include <thread>

namespace Sailor::Utils
{
	SAILOR_API void SetThreadName(size_t dwThreadID, const std::string& threadName);
	SAILOR_API void SetThreadName(const std::string& threadName);
	SAILOR_API void SetThreadName(std::thread* thread, const std::string& threadName);
	SAILOR_API std::string GetCurrentThreadName();
}
