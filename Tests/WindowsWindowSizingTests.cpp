#include "Platform/Win32/Window.h"
#include <windows.h>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	void CheckClientExtent(Sailor::Win32::Window& window, int width, int height)
	{
		RECT client{};
		Require(GetClientRect(window.GetHWND(), &client), "client rectangle must be readable");
		Require(client.right - client.left == width && client.bottom - client.top == height,
			"the physical client area must match the requested render dimensions");
		Require(window.GetWidth() == width && window.GetHeight() == height,
			"the renderer must receive client dimensions without DPI scaling or window borders");
		window.RecalculateWindowSize();
		Require(window.GetWidth() == width && window.GetHeight() == height,
			"recalculating the window size must preserve client dimensions");
	}
}

int main()
{
	try
	{
		Require(AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
			DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
			"the executable manifest must enable physical-pixel coordinates before window creation");
		Sailor::Win32::Window window;
		Require(window.Create("Sailor window sizing test", "SailorWindowSizingTest", 640, 400),
			"the native test window must be created");
		window.Show(false);
		Require(AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(window.GetHWND()),
			DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), "the native window must inherit PerMonitorV2");
		CheckClientExtent(window, 640, 400);
		std::cout << "[PASS] Startup client extent at DPI " << GetDpiForWindow(window.GetHWND()) << '\n';

		window.ChangeWindowSize(1280, 800);
		window.Show(false);
		CheckClientExtent(window, 1280, 800);
		window.ChangeWindowSize(853, 479);
		window.Show(false);
		CheckClientExtent(window, 853, 479);
		std::cout << "[PASS] Resizing preserves exact physical client dimensions\n";

		RECT suggested{ 100, 100, 740, 500 };
		const UINT dpi = GetDpiForWindow(window.GetHWND());
		Require(AdjustWindowRectExForDpi(&suggested,
			static_cast<DWORD>(GetWindowLongPtr(window.GetHWND(), GWL_STYLE)), FALSE,
			static_cast<DWORD>(GetWindowLongPtr(window.GetHWND(), GWL_EXSTYLE)), dpi),
			"a DPI change must have a valid suggested outer rectangle");
		SendMessage(window.GetHWND(), WM_DPICHANGED, MAKELONG(dpi, dpi),
			reinterpret_cast<LPARAM>(&suggested));
		CheckClientExtent(window, 640, 400);
		RECT actual{};
		Require(GetWindowRect(window.GetHWND(), &actual) && EqualRect(&actual, &suggested),
			"DPI changes must apply the suggested window rectangle");
		std::cout << "[PASS] DPI change updates window and renderer dimensions together\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "[FAIL] " << error.what() << '\n';
		return 1;
	}
	return 0;
}
