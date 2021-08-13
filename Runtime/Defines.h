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

#ifndef _WINDEF_
typedef unsigned long DWORD;
#endif