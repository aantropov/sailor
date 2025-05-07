#pragma once

struct IUnknown; // Workaround for "combaseapi.h(229): error C2187: syntax error: 'identifier' was unexpected here" when using /permissive-

#ifndef _SAILOR_IMPORT_
# define SAILOR_API __declspec(dllexport)
#else
# define SAILOR_API __declspec(dllimport)
#endif

#include <cassert>

#if defined(BUILD_WITH_TRACY_PROFILER)
#include "tracy/Tracy.hpp"
#endif

#define GLM_FORCE_CXX17
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL
#define GLM_GTC_quaternion
#define GLM_GTX_rotate_vector
#define GLM_FORCE_RIGHT_HANDED
#define GLM_SWIZZLE_XYZW
#define GLM_FORCE_SWIZZLE
#define GLM_GTC_random
#define GLM_GTX_matrix_transform_2d
//#define GLM_FORCE_DEFAULT_ALIGNED_GENTYPES

#ifndef _WINDEF_
typedef unsigned long DWORD;
#endif

#if defined(SAILOR_PROFILING_ENABLE) && defined(BUILD_WITH_TRACY_PROFILER)
#define SAILOR_PROFILE_FUNCTION() ZoneScoped

#define SAILOR_PROFILE_SCOPE(Msg) ZoneScopedN(Msg)
/* Should we use instead? ZoneTransientN(___tracy_scoped_zone, Msg, true) */

#define SAILOR_PROFILE_TEXT(Msg) ZoneText(Msg, strlen(Msg))
#define SAILOR_PROFILE_BLOCK(HashMsg) //SAILOR_PROFILE_BLOCK_STR(HashMsg.ToString().c_str())
#define SAILOR_PROFILE_END_BLOCK(HashMsg) //SAILOR_PROFILE_END_BLOCK_STR(HashMsg.ToString().c_str())
#define SAILOR_PROFILE_END_FRAME() FrameMark
#define SAILOR_PROFILE_THREAD_NAME(ThreadName) tracy::SetThreadName(ThreadName)
#define SAILOR_PROFILE_ALLOC(ptr, size) TracyAlloc(ptr, size)
#define SAILOR_PROFILE_FREE(ptr) TracyFree(ptr)

#else 
#define SAILOR_PROFILE_FUNCTION()
#define SAILOR_PROFILE_SCOPE(Msg)
#define SAILOR_PROFILE_TEXT(Msg)
#define SAILOR_PROFILE_BLOCK(HashMsg)
#define SAILOR_PROFILE_END_BLOCK(HashMsg)
#define SAILOR_PROFILE_END_FRAME()
#define SAILOR_PROFILE_THREAD_NAME(ThreadName)
#define SAILOR_PROFILE_ALLOC(ptr, size)
#define SAILOR_PROFILE_FREE(ptr)
#endif

#define SAILOR_EDITOR

#define MAGIC_ENUM_RANGE_MIN 0
#define MAGIC_ENUM_RANGE_MAX 256
#include <magic_enum/magic_enum.hpp>

#define checkAtCompileTime(expr, msg) static_assert(expr, #msg);
#define check(expr) assert(expr);
#define ensure(expr, msg, ...) { static bool bOnce = false; if(!(expr) && !bOnce) { SAILOR_LOG(#msg, __VA_ARGS__); bOnce = true; }}

//Memory
namespace Sailor::Memory
{
#ifdef SAILOR_MEMORY_USE_LOCK_FREE_HEAP_ALLOCATOR_AS_DEFAULT
	using DefaultGlobalAllocator = class LockFreeHeapAllocator;
#else
	using DefaultGlobalAllocator = class MallocAllocator;
#endif
}