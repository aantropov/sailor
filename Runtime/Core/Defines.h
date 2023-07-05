#pragma once

struct IUnknown; // Workaround for "combaseapi.h(229): error C2187: syntax error: 'identifier' was unexpected here" when using /permissive-

#ifndef _SAILOR_IMPORT_
# define SAILOR_API __declspec(dllexport)
#else
# define SAILOR_API __declspec(dllimport)
#endif

#include <cassert>

#if defined(BUILD_WITH_EASY_PROFILER)
#include "easy_profiler/easy_profiler_core/include/easy/profiler.h"
#endif

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL
#define GLM_GTC_quaternion
#define GLM_GTX_rotate_vector
#define GLM_FORCE_RIGHT_HANDED
#define GLM_SWIZZLE_XYZW
#define GLM_FORCE_SWIZZLE
#define GLM_GTC_random
//#define GLM_FORCE_DEFAULT_ALIGNED_GENTYPES

#ifndef _WINDEF_
typedef unsigned long DWORD;
#endif

#ifdef SAILOR_PROFILING_ENABLE
#define SAILOR_PROFILE_FUNCTION() EASY_FUNCTION()
#define SAILOR_PROFILE_BLOCK(Msg, ...) EASY_BLOCK(Msg, __VA_ARGS__)
#define SAILOR_PROFILE_END_BLOCK() EASY_END_BLOCK
#else
#define SAILOR_PROFILE_FUNCTION()
#define SAILOR_PROFILE_BLOCK(Msg, ...)
#define SAILOR_PROFILE_END_BLOCK()
#endif

#define SAILOR_EDITOR

#define MAGIC_ENUM_RANGE_MIN 0
#define MAGIC_ENUM_RANGE_MAX 256
#include "magic_enum/include/magic_enum.hpp"

#ifndef min
#define min(a,b) ((a) < (b) ? (a) : (b))
#endif

#ifndef max
#define max(a,b) ((a) > (b) ? (a) : (b))
#endif

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