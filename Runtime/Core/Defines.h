#pragma once

struct IUnknown; // Workaround for "combaseapi.h(229): error C2187: syntax error: 'identifier' was unexpected here" when using /permissive-

#if defined(_WIN32)
# ifndef _SAILOR_IMPORT_
#  define SAILOR_API __declspec(dllexport)
# else
#  define SAILOR_API __declspec(dllimport)
# endif
#else
# if defined(__GNUC__) || defined(__clang__)
#  define SAILOR_API __attribute__((visibility("default")))
# else
#  define SAILOR_API
# endif
#endif

#if !defined(__forceinline)
# define __forceinline
#endif

#include <cassert>
#include <cstdio>
#if !defined(_WIN32)
#include <thread>
#include <chrono>
#include <cstdlib>
#include <alloca.h>
#endif

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

#if !defined(_WIN32)
typedef void* HWND;
typedef void* HINSTANCE;
typedef void* HDC;
typedef const char* LPCSTR;
typedef unsigned int UINT;
typedef unsigned long WPARAM;
typedef long LPARAM;
typedef long LRESULT;
#ifndef CALLBACK
#define CALLBACK
#endif
struct RECT
{
	long left;
	long top;
	long right;
	long bottom;
};

#ifndef VK_LBUTTON
#define VK_LBUTTON 0x01
#endif
#ifndef VK_RBUTTON
#define VK_RBUTTON 0x02
#endif
#ifndef VK_MBUTTON
#define VK_MBUTTON 0x04
#endif
#ifndef VK_SHIFT
#define VK_SHIFT 0x10
#endif
#ifndef VK_CONTROL
#define VK_CONTROL 0x11
#endif
#ifndef VK_ESCAPE
#define VK_ESCAPE 0x1B
#endif
#ifndef VK_F5
#define VK_F5 0x74
#endif
#ifndef VK_F6
#define VK_F6 0x75
#endif

inline DWORD GetCurrentThreadId()
{
	return (DWORD)std::hash<std::thread::id>{}(std::this_thread::get_id());
}

inline bool IsDebuggerPresent()
{
	return false;
}

inline void Sleep(uint32_t ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

#ifndef _malloca
#define _malloca(size) alloca(size)
#endif
#ifndef _freea
#define _freea(ptr) (void)(ptr)
#endif

#ifndef sprintf_s
#define sprintf_s(buffer, sizeOfBuffer, format, ...) std::snprintf(buffer, sizeOfBuffer, format, __VA_ARGS__)
#endif
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
#define ensure(expr, msg, ...) { static bool bOnce = false; if(!(expr) && !bOnce) { SAILOR_LOG("%s", #msg); bOnce = true; }}

//Memory
namespace Sailor::Memory
{
#ifdef SAILOR_MEMORY_USE_LOCK_FREE_HEAP_ALLOCATOR_AS_DEFAULT
	using DefaultGlobalAllocator = class LockFreeHeapAllocator;
#else
	using DefaultGlobalAllocator = class MallocAllocator;
#endif
}
