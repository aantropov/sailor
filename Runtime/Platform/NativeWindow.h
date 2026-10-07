#pragma once

#if defined(__APPLE__)
#include "Platform/Mac/Window.h"
#else
#include "Platform/Win32/Window.h"
#endif

namespace Sailor::Platform
{
#if defined(__APPLE__)
	using NativeWindow = Mac::Window;
#else
	using NativeWindow = Win32::Window;
#endif
}
