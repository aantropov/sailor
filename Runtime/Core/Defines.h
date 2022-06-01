#pragma once

struct IUnknown; // Workaround for "combaseapi.h(229): error C2187: syntax error: 'identifier' was unexpected here" when using /permissive-

#ifndef _SAILOR_IMPORT_
# define SAILOR_API __declspec(dllexport)
#else
# define SAILOR_API __declspec(dllimport)
#endif

#if defined(BUILD_WITH_EASY_PROFILER)
#include "easy_profiler/include/easy/profiler.h"
#endif

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL
#define GLM_GTC_quaternion
#define GLM_GTX_rotate_vector
#define GLM_FORCE_RIGHT_HANDED
#define GLM_SWIZZLE_XYZW
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

//Memory
namespace Sailor::Memory
{
#ifdef SAILOR_USE_LOCK_FREE_HEAP_ALLOCATOR_AS_DEFAULT
	using DefaultGlobalAllocator = class LockFreeHeapAllocator;
#else
	using DefaultGlobalAllocator = class MallocAllocator;
#endif
}