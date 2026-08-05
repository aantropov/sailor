#include "Physics/JoltRuntime.h"
#include "Core/LogMacros.h"
#include "Tasks/Tasks.h"
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
#include <cstdarg>
#include <cstdio>

using namespace Sailor;

namespace
{
	void TraceJolt(const char* format, ...)
	{
		char buffer[2048]{};
		va_list arguments;
		va_start(arguments, format);
		vsnprintf(buffer, sizeof(buffer), format, arguments);
		va_end(arguments);
		SAILOR_LOG("Jolt: %s", buffer);
	}

#if defined(JPH_ENABLE_ASSERTS)
	bool AssertJolt(
		const char* expression,
		const char* message,
		const char* file,
		JPH::uint line)
	{
		SAILOR_LOG_ERROR(
			"Jolt assertion at %s:%u: %s (%s)",
			file,
			line,
			expression,
			message ? message : "");
		return true;
	}
#endif
}

Physics::JoltRuntime::JoltRuntime()
{
	JPH::RegisterDefaultAllocator();
	JPH::Trace = TraceJolt;
#if defined(JPH_ENABLE_ASSERTS)
	JPH::AssertFailed = AssertJolt;
#endif
	check(JPH::Factory::sInstance == nullptr);
	JPH::Factory::sInstance = new JPH::Factory();
	JPH::RegisterTypes();
}

Physics::JoltRuntime::~JoltRuntime()
{
	JPH::UnregisterTypes();
	delete JPH::Factory::sInstance;
	JPH::Factory::sInstance = nullptr;
	JPH::Trace = nullptr;
#if defined(JPH_ENABLE_ASSERTS)
	JPH::AssertFailed = nullptr;
#endif
}
