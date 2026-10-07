#pragma once

#include "Core/Defines.h"
#include "Core/StringHash.h"

#include <cstddef>
#include <string_view>
#include <thread>

namespace Sailor::Utils
{
	SAILOR_API void SetThreadName(size_t dwThreadID, std::string_view threadName);
	SAILOR_API void SetThreadName(std::string_view threadName);
	SAILOR_API void SetThreadName(std::thread* thread, std::string_view threadName);
	SAILOR_API StringHash GetCurrentThreadName();
}
