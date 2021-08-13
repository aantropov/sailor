#pragma once

#include "ExportDef.h"
#include <wtypes.h>
#include <atomic>

namespace Sailor
{
	LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

	class Window
	{
		const std::string WindowClassName = "SailorViewport";

		HINSTANCE m_hInstance = nullptr;
		HWND      m_hWnd = nullptr;
		HDC       m_hDC = nullptr;

		int m_width = 1024;
		int m_height = 768;

		std::atomic<bool> m_bIsFullscreen = false;
		std::atomic<bool> m_bIsActive = false;
		std::atomic<bool> m_bIsRunning = false;
		std::atomic<bool> m_bIsIconic = false;

	public:

		SAILOR_API Window() = default;
		SAILOR_API ~Window() = default;

		// Getters
		SAILOR_API bool IsActive() const { return m_bIsActive; }
		SAILOR_API bool IsRunning() const { return m_bIsRunning; }
		SAILOR_API bool IsFullscreen() const { return m_bIsFullscreen; }
		SAILOR_API bool IsIconic() const;

		// Setters
		SAILOR_API void SetActive(bool value) { m_bIsActive = value; }
		SAILOR_API void SetRunning(bool value) { m_bIsRunning = value; }
		SAILOR_API void SetFullscreen(bool value) { m_bIsFullscreen = value; }
		SAILOR_API void SetWindowTitle(LPCWSTR lString) { SetWindowText(m_hWnd, lString); }

		// Getters for handles
		SAILOR_API HWND GetHWND() const { return m_hWnd; }

		SAILOR_API HDC GetHDC() const { return m_hDC; }
		SAILOR_API HINSTANCE GetHINSTANCE() const { return m_hInstance; }

		//Getters for parameters
		SAILOR_API int32_t GetWidth() const { return m_width; }
		SAILOR_API int32_t GetHeight() const { return m_height; }

		// Create window
		SAILOR_API bool Create(LPCWSTR title = L"Sailor", int32_t width = 1920, int32_t height = 1080, bool bIsFullScreen = false);
		SAILOR_API void Destroy();

		// Window size
		SAILOR_API void RecalculateWindowSize();
		SAILOR_API void ChangeWindowSize(int32_t width, int32_t height, bool bIsFullScreen = false);

	private:

		SAILOR_API void SetIsIconic(bool value) { m_bIsIconic = value; }

		friend LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	};

}