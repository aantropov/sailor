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

		Window() = default;
		~Window() = default;

		// Getters
		bool IsActive() const { return bIsActive; }
		bool IsRunning() const { return bIsRunning; }
		bool IsFullscreen() const { return bIsFullscreen; }

		// Setters
		void SetActive(bool value) { bIsActive = value; }
		void SetRunning(bool value) { bIsRunning = value; }
		void SetFullscreen(bool value) { bIsFullscreen = value; }

		void SetWindowTitle(LPCWSTR lString) { SetWindowText(g_hWnd, lString); }

		// Getters for handles
		HWND GetHWND() const { return g_hWnd; }
		HDC GetHDC() const { return g_hDC; }

		//Getters for parameters
		int GetWidth() const { return width; }
		int GetHeight() const { return height; }

		// Create window
		bool Create(LPCWSTR title = L"Sailor", int width = 1920, int height = 1080, bool bIsFullScreen = false);

		// Destroy window
		void Destroy();

		// Set window size
		void SetSize(int width, int height, bool bIsFullScreen = false);
	};

	LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
}