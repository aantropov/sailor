#pragma once

#ifdef _WIN32
#include "Platform/Win32/Window.h"
namespace Sailor::Platform
{
    using Window = Sailor::Win32::Window;
}
#else
// Minimal stub implementation used when building on non-Windows platforms. This
// provides the API surface required by the engine without any real
// functionality so the code can still be compiled and linked.
#include "Math/Math.h"
namespace Sailor::Platform
{
    struct RECT;
    using HWND = void*;
    using HDC = void*;
    using HINSTANCE = void*;

    class Window
    {
    public:
        bool Create(const char* = "", const char* = "", int32_t = 0, int32_t = 0,
                     bool = false, bool = false, void* = nullptr) { return false; }
        void Destroy() {}

        bool IsShown() const { return true; }
        bool IsResizing() const { return false; }
        bool IsActive() const { return true; }
        bool IsRunning() const { return true; }
        bool IsFullscreen() const { return false; }
        bool IsIconic() const { return false; }

        void SetActive(bool) {}
        void SetRunning(bool) {}
        void SetFullscreen(bool) {}
        void SetWindowTitle(const char*) {}

        void* GetHWND() const { return nullptr; }
        void* GetHDC() const { return nullptr; }
        void* GetHINSTANCE() const { return nullptr; }

        int32_t GetWidth() const { return 0; }
        int32_t GetHeight() const { return 0; }
        bool IsVsyncRequested() const { return false; }

        glm::ivec2 GetRenderArea() const { return {}; }
        void SetRenderArea(const glm::ivec2&) {}

        bool IsParentWindowValid() const { return true; }
        void TrackParentWindowPosition(const RECT&) {}

        void Show(bool) {}

        glm::ivec2 GetCenterPointScreen() const { return {}; }
        glm::ivec2 GetCenterPointClient() const { return {}; }
        void SetWindowPos(const RECT&) {}
        void RecalculateWindowSize() {}
        void ChangeWindowSize(int32_t, int32_t, bool = false) {}

        static void ProcessWin32Msgs() {}
    };
}
#endif
