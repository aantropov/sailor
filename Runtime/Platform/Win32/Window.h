#pragma once

#include "Core/Defines.h"
#if defined(_WIN32)
#include <wtypes.h>
#endif
#include <atomic>
#include "Math/Math.h"
#include "Containers/Containers.h"

namespace Sailor::Win32
{
	LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
}

namespace Sailor::Platform
{
	class Window
	{
	public:

		SAILOR_API virtual ~Window() = default;

		SAILOR_API virtual bool IsShown() const = 0;
		SAILOR_API virtual bool IsResizing() const = 0;
		SAILOR_API virtual bool IsActive() const = 0;
		SAILOR_API virtual bool IsRunning() const = 0;
		SAILOR_API virtual bool IsFullscreen() const = 0;
		SAILOR_API virtual bool IsIconic() const = 0;

		SAILOR_API virtual void SetActive(bool value) = 0;
		SAILOR_API virtual void SetRunning(bool value) = 0;
		SAILOR_API virtual void SetFullscreen(bool value) = 0;
		SAILOR_API virtual void SetWindowTitle(LPCSTR lString) = 0;

		SAILOR_API virtual HWND GetHWND() const = 0;
		SAILOR_API virtual HDC GetHDC() const = 0;
		SAILOR_API virtual HINSTANCE GetHINSTANCE() const = 0;
#if defined(__APPLE__)
		SAILOR_API virtual void* GetMetalLayer() const = 0;
		SAILOR_API virtual void* GetNativeView() const = 0;
#endif

		SAILOR_API virtual int32_t GetWidth() const = 0;
		SAILOR_API virtual int32_t GetHeight() const = 0;
		SAILOR_API virtual bool IsVsyncRequested() const = 0;

		SAILOR_API virtual bool Create(LPCSTR title = "Sailor", LPCSTR className = "SailorViewport", int32_t width = 1920, int32_t height = 1080, bool bIsFullScreen = false, bool bRequestVsync = false, HWND parentHwnd = NULL) = 0;
		SAILOR_API virtual void Destroy() = 0;

		SAILOR_API virtual glm::ivec2 GetRenderArea() const = 0;
		SAILOR_API virtual void SetRenderArea(const glm::ivec2& renderArea) = 0;

		SAILOR_API virtual bool IsParentWindowValid() const = 0;
		SAILOR_API virtual void TrackParentWindowPosition(const RECT& viewport) = 0;

		SAILOR_API virtual void Show(bool bShowWindow) = 0;

		SAILOR_API virtual glm::ivec2 GetCenterPointScreen() const = 0;
		SAILOR_API virtual glm::ivec2 GetCenterPointClient() const = 0;
		SAILOR_API virtual void SetWindowPos(const RECT& rect) = 0;
		SAILOR_API virtual void RecalculateWindowSize() = 0;
		SAILOR_API virtual void ChangeWindowSize(int32_t width, int32_t height, bool bIsFullScreen = false) = 0;
		SAILOR_API virtual void ProcessSystemMessages() = 0;
	};
}

namespace Sailor::Win32
{
	class Window
		: public Platform::Window
	{
		HINSTANCE m_hInstance = nullptr;
		HWND      m_hWnd = nullptr;
		HDC       m_hDC = nullptr;

		HWND      m_parentHwnd = nullptr;

		std::atomic<int> m_width = 1024;
		std::atomic<int> m_height = 768;
		std::string m_windowClassName;

		std::atomic<bool> m_bIsShown = true;
		std::atomic<bool> m_bIsFullscreen = false;
		std::atomic<bool> m_bIsActive = false;
		std::atomic<bool> m_bIsRunning = false;
		std::atomic<bool> m_bIsIconic = false;
		std::atomic<bool> m_bIsResizing = false;
		std::atomic<bool> m_bIsVsyncRequested = false;

		glm::ivec2 m_renderArea{};

	public:

		SAILOR_API Window() = default;
		SAILOR_API ~Window() override = default;

		SAILOR_API bool IsShown() const override { return m_bIsShown; }
		SAILOR_API bool IsResizing() const override { return m_bIsResizing; }
		SAILOR_API bool IsActive() const override { return m_bIsActive; }
		SAILOR_API bool IsRunning() const override { return m_bIsRunning; }
		SAILOR_API bool IsFullscreen() const override { return m_bIsFullscreen; }
		SAILOR_API bool IsIconic() const override;

		SAILOR_API void SetActive(bool value) override { m_bIsActive = value; }
		SAILOR_API void SetRunning(bool value) override { m_bIsRunning = value; }
		SAILOR_API void SetFullscreen(bool value) override { m_bIsFullscreen = value; }
		SAILOR_API void SetWindowTitle(LPCSTR lString) override {
#if defined(_WIN32)
			SetWindowText(m_hWnd, lString);
#else
			(void)lString;
#endif
		}

		SAILOR_API HWND GetHWND() const override { return m_hWnd; }
		SAILOR_API HDC GetHDC() const override { return m_hDC; }
		SAILOR_API HINSTANCE GetHINSTANCE() const override { return m_hInstance; }
#if defined(__APPLE__)
		SAILOR_API void* GetMetalLayer() const override;
		SAILOR_API void* GetNativeView() const override;
#endif

		SAILOR_API int32_t GetWidth() const override { return m_width; }
		SAILOR_API int32_t GetHeight() const override { return m_height; }
		SAILOR_API bool IsVsyncRequested() const override { return m_bIsVsyncRequested; }

		SAILOR_API bool Create(LPCSTR title = "Sailor", LPCSTR className = "SailorViewport", int32_t width = 1920, int32_t height = 1080, bool bIsFullScreen = false, bool bRequestVsync = false, HWND parentHwnd = NULL) override;
		SAILOR_API void Destroy() override;

		SAILOR_API glm::ivec2 GetRenderArea() const override { return m_renderArea; }
		SAILOR_API void SetRenderArea(const glm::ivec2& renderArea) override { m_renderArea = renderArea; }

		SAILOR_API bool IsParentWindowValid() const override;
		SAILOR_API void TrackParentWindowPosition(const RECT& viewport) override;

		SAILOR_API void Show(bool bShowWindow) override;

		// Window size
		SAILOR_API glm::ivec2 GetCenterPointScreen() const override;
		SAILOR_API glm::ivec2 GetCenterPointClient() const override;
		SAILOR_API void SetWindowPos(const RECT& rect) override;
		SAILOR_API void RecalculateWindowSize() override;
		SAILOR_API void ChangeWindowSize(int32_t width, int32_t height, bool bIsFullScreen = false) override;
		SAILOR_API void ProcessSystemMessages() override;

		SAILOR_API static void ProcessWin32Msgs();
#if defined(__APPLE__)
		SAILOR_API static void ProcessMacMsgs();
#endif

	private:

		RECT m_viewport{};

		SAILOR_API void SetIsIconic(bool value) { m_bIsIconic = value; }

		static TVector<Window*> g_windows;

		friend LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	};

}
