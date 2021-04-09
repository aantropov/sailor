#pragma once

#include "ExportDef.h"
#include <wtypes.h>

namespace Sailor
{
	class Window
	{
		const std::string WindowClassName = "SailorViewport";

		HINSTANCE g_hInstance = nullptr;
		HWND      g_hWnd = nullptr;
		HDC       g_hDC = nullptr;

		int width = 1024;
		int height = 768;

		bool bIsFullscreen = false;
		bool bIsActive = false;
		bool bIsRunning = false;

	public:

		SAILOR_API Window() = default;
		SAILOR_API ~Window() = default;

		// Getters
		SAILOR_API bool IsActive() const { return bIsActive; }
		SAILOR_API bool IsRunning() const { return bIsRunning; }
		SAILOR_API bool IsFullscreen() const { return bIsFullscreen; }

		// Setters
		SAILOR_API void SetActive(bool value) { bIsActive = value; }
		SAILOR_API void SetRunning(bool value) { bIsRunning = value; }
		SAILOR_API void SetFullscreen(bool value) { bIsFullscreen = value; }

		SAILOR_API void SetWindowTitle(LPCWSTR lString) { SetWindowText(g_hWnd, lString); }

		// Getters for handles
		SAILOR_API HWND GetHWND() const { return g_hWnd; }
		SAILOR_API HDC GetHDC() const { return g_hDC; }
		SAILOR_API HINSTANCE GetHINSTANCE() const { return g_hInstance; }

		//Getters for parameters
		SAILOR_API int GetWidth() const { return width; }
		SAILOR_API int GetHeight() const { return height; }

		// Create window
		SAILOR_API bool Create(LPCWSTR title = L"Sailor", int width = 1920, int height = 1080, bool bIsFullScreen = false);

		// Destroy window
		SAILOR_API void Destroy();

		// Set window size
		SAILOR_API void SetSize(int width, int height, bool bIsFullScreen = false);
	};

	LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
}