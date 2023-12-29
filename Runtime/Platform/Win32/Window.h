#pragma once

#include "Core/Defines.h"
#include <wtypes.h>
#include <atomic>
#include "Math/Math.h"
#include "Containers/Containers.h"

namespace Sailor::Win32
{
	LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

	class Window
	{
		HINSTANCE m_hInstance = nullptr;
		HWND      m_hWnd = nullptr;
		HDC       m_hDC = nullptr;


		std::atomic<int> m_width = 1024;
		std::atomic<int> m_height = 768;
		std::string m_windowClassName;

		std::atomic<bool> m_bIsFullscreen = false;
		std::atomic<bool> m_bIsActive = false;
		std::atomic<bool> m_bIsRunning = false;
		std::atomic<bool> m_bIsIconic = false;
		std::atomic<bool> m_bIsResizing = false;
		std::atomic<bool> m_bIsVsyncRequested = false;

		glm::ivec2 m_renderArea{};

	public:

		SAILOR_API Window() = default;
		SAILOR_API ~Window() = default;

		SAILOR_API bool IsResizing() const { return m_bIsResizing; }
		SAILOR_API bool IsActive() const { return m_bIsActive; }
		SAILOR_API bool IsRunning() const { return m_bIsRunning; }
		SAILOR_API bool IsFullscreen() const { return m_bIsFullscreen; }
		SAILOR_API bool IsIconic() const;

		SAILOR_API void SetActive(bool value) { m_bIsActive = value; }
		SAILOR_API void SetRunning(bool value) { m_bIsRunning = value; }
		SAILOR_API void SetFullscreen(bool value) { m_bIsFullscreen = value; }
		SAILOR_API void SetWindowTitle(LPCSTR lString) { SetWindowText(m_hWnd, lString); }

		SAILOR_API HWND GetHWND() const { return m_hWnd; }
		SAILOR_API HDC GetHDC() const { return m_hDC; }
		SAILOR_API HINSTANCE GetHINSTANCE() const { return m_hInstance; }

		SAILOR_API int32_t GetWidth() const { return m_width; }
		SAILOR_API int32_t GetHeight() const { return m_height; }
		SAILOR_API bool IsVsyncRequested() const { return m_bIsVsyncRequested; }

		SAILOR_API void TrackExternalViewport(HWND hwnd);

		SAILOR_API bool CreateContext(HWND hwnd);
		SAILOR_API bool Create(LPCSTR title = "Sailor", LPCSTR className = "SailorViewport", int32_t width = 1920, int32_t height = 1080, bool bIsFullScreen = false, bool bRequestVsync = false, HWND parentHwnd = NULL);
		SAILOR_API void Destroy();

		SAILOR_API glm::ivec2 GetRenderArea() const { return m_renderArea; }
		SAILOR_API void SetRenderArea(const glm::ivec2& renderArea) { m_renderArea = renderArea; }

		// Window size
		SAILOR_API glm::ivec2 GetCenterPointScreen() const;
		SAILOR_API glm::ivec2 GetCenterPointClient() const;
		SAILOR_API void RecalculateWindowSize();
		SAILOR_API void ChangeWindowSize(int32_t width, int32_t height, bool bIsFullScreen = false);

		SAILOR_API static void ProcessWin32Msgs();

	private:

		SAILOR_API void SetIsIconic(bool value) { m_bIsIconic = value; }

		static TVector<Window*> g_windows;

		friend LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	};

}