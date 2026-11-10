#pragma once

#if defined(__APPLE__)
#include "Platform/Mac/Window.h"
#elif defined(__linux__)
#include "Platform/Linux/Window.h"
#else
#include "Platform/Win32/Window.h"
#endif

namespace Sailor::Platform
{
#if defined(__APPLE__)
	using NativeWindow = Mac::Window;
#elif defined(__linux__)
	using NativeWindow = Linux::Window;
#else
	using NativeWindow = Win32::Window;
#endif
}
