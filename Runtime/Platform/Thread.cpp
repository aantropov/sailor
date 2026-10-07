#include "Thread.h"
#include "Core/Utils.h"
#include "Sailor.h"
#include "Tasks/Scheduler.h"

#include <format>

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace Sailor;
using namespace Sailor::Utils;

StringHash Utils::GetCurrentThreadName()
{
	if (App::GetSubmodule<Tasks::Scheduler>()->IsMainThread())
	{
		return "Thread Main"_h;
	}
	else if (App::GetSubmodule<Tasks::Scheduler>()->IsRendererThread())
	{
		return "Thread Render"_h;
	}
	else
	{
		thread_local const auto name = StringHash::Runtime(std::format("Thread {}", GetCurrentThreadId()));
		return name;
	}
}

void Utils::SetThreadName(size_t dwThreadID, std::string_view threadName)
{
#if defined(_WIN32)
	SetThreadDescription(
		(HANDLE)(dwThreadID),
		UTF8_to_wchar(threadName).c_str()
	);
#else
	(void)dwThreadID;
	(void)threadName;
#endif
}

void Utils::SetThreadName(std::string_view threadName)
{
#if defined(_WIN32)
	SetThreadDescription(
		GetCurrentThread(),
		UTF8_to_wchar(threadName).c_str()
	);
#else
	(void)threadName;
#endif
}

void Utils::SetThreadName(std::thread* thread, std::string_view threadName)
{
#if defined(_WIN32)
	SetThreadDescription(
		(HANDLE)thread->native_handle(),
		UTF8_to_wchar(threadName).c_str()
	);
#else
	(void)thread;
	(void)threadName;
#endif
}
