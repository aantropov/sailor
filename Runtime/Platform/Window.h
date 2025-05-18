#pragma once

#ifdef _WIN32
#include "Platform/Win32/Window.h"
namespace Sailor::Platform
{
    using Window = Sailor::Win32::Window;
}
#else
namespace Sailor::Platform
{
    class Window
    {
        // TODO: Implement window for non-Windows platforms
    };
}
#endif
