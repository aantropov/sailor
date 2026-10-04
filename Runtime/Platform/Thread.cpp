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

std::string Utils::GetCurrentThreadName()
{
	if (App::GetSubmodule<Tasks::Scheduler>()->IsMainThread())
	{
		return std::string("Thread Main");
	}
	else if (App::GetSubmodule<Tasks::Scheduler>()->IsRendererThread())
	{
		return std::string("Thread Render");
	}
	else
	{
		return std::format("Thread {}", GetCurrentThreadId());
	}
}

void Utils::SetThreadName(size_t dwThreadID, const std::string& threadName)
{
#if defined(_WIN32)
	SetThreadDescription(
		(HANDLE)(dwThreadID),
		UTF8_to_wchar(threadName.c_str()).c_str()
	);
#else
	(void)dwThreadID;
	(void)threadName;
#endif
}

void Utils::SetThreadName(const std::string& threadName)
{
#if defined(_WIN32)
	SetThreadDescription(
		GetCurrentThread(),
		UTF8_to_wchar(threadName.c_str()).c_str()
	);
#else
	(void)threadName;
#endif
}

void Utils::SetThreadName(std::thread* thread, const std::string& threadName)
{
#if defined(_WIN32)
	SetThreadDescription(
		(HANDLE)thread->native_handle(),
		UTF8_to_wchar(threadName.c_str()).c_str()
	);
#else
	(void)thread;
	(void)threadName;
#endif
}
