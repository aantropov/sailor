struct IUnknown; // Workaround for "combaseapi.h(229): error C2187: syntax error: 'identifier' was unexpected here" when using /permissive-

#pragma once

#ifndef _SAILOR_IMPORT_
# define SAILOR_API __declspec(dllexport)
#else
# define SAILOR_API __declspec(dllimport)
#endif

#ifndef BUILD_WITH_EASY_PROFILER
#define BUILD_WITH_EASY_PROFILER
#endif

#include <easy/profiler.h>
#pragma comment(lib, "easy_profiler.lib")

#define SAILOR_PROFILE_FUNCTION() EASY_FUNCTION()
#define SAILOR_PROFILE_BLOCK(Msg, ...) EASY_BLOCK(Msg, __VA_ARGS__)
#define SAILOR_PROFILE_END_BLOCK() EASY_END_BLOCK

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL
#define GLM_GTC_quaternion
#define GLM_GTX_rotate_vector
#define GLM_FORCE_RIGHT_HANDED
//#define GLM_FORCE_DEFAULT_ALIGNED_GENTYPES

#define NOMINMAX

#ifndef _WINDEF_
typedef unsigned long DWORD;
#endif

#define VULKAN