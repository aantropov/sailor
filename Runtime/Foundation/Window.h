#pragma once

#include "ExportDef.h"
#include <wtypes.h>

namespace Sailor
{
	class AWindow
	{
		const std::string WindowClassName = "SailorViewport";

		HINSTANCE g_hInstance = nullptr;
		HWND      g_hWnd = nullptr;
		HDC       g_hDC = nullptr;		

		int Width = 1024;
		int Height = 768;

		bool bIsFullscreen = false;
		bool bIsActive = false;
		bool bIsRunning = false;

	public:

		AWindow() = default;
		~AWindow() = default;

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
		int GetWidth() const { return Width; }
		int GetHeight() const { return Height; }

		// Create window
		bool Create(LPCWSTR title = L"Sailor", int Width = 1920, int Height = 1080, bool bIsFullScreen = false);

		// Destroy window
		void Destroy();

		// Set window size
		void SetSize(int Width, int Height, bool bIsFullScreen = false);
	};

	LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
}