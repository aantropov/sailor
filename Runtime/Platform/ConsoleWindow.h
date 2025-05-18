#pragma once

#ifdef _WIN32
#include "Platform/Win32/ConsoleWindow.h"
namespace Sailor::Platform
{
    using ConsoleWindow = Sailor::Win32::ConsoleWindow;
}
#else
namespace Sailor::Platform
{
    class ConsoleWindow
    {
    public:
        static void Initialize(bool) {}
        void OpenWindow(const wchar_t*) {}
        void CloseWindow() {}
        void Update() {}
    };
}
#endif
