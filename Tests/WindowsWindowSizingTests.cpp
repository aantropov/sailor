#include "Platform/Win32/Window.h"
#include <windows.h>
#include <algorithm>
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
		std::cout << "[INFO] Expected client " << width << 'x' << height
			<< ", native " << client.right - client.left << 'x' << client.bottom - client.top
			<< ", renderer " << window.GetWidth() << 'x' << window.GetHeight()
			<< ", DPI " << GetDpiForWindow(window.GetHWND()) << '\n';
		Require(client.right - client.left == width && client.bottom - client.top == height,
			"the physical client area must match the expected render dimensions");
		Require(window.GetWidth() == width && window.GetHeight() == height,
			"the renderer must receive client dimensions without DPI scaling or window borders");
		window.RecalculateWindowSize();
		Require(window.GetWidth() == width && window.GetHeight() == height,
			"recalculating the window size must preserve client dimensions");
	}

	SIZE GetMaximumClientExtent(HWND window)
	{
		const UINT dpi = GetDpiForWindow(window);
		RECT frame{};
		Require(AdjustWindowRectExForDpi(&frame,
			static_cast<DWORD>(GetWindowLongPtr(window, GWL_STYLE)), FALSE,
			static_cast<DWORD>(GetWindowLongPtr(window, GWL_EXSTYLE)), dpi),
			"the native window frame must have valid physical dimensions");
		const SIZE maximum{
			GetSystemMetricsForDpi(SM_CXMAXTRACK, dpi) - (frame.right - frame.left),
			GetSystemMetricsForDpi(SM_CYMAXTRACK, dpi) - (frame.bottom - frame.top)
		};
		Require(maximum.cx > 0 && maximum.cy > 0,
			"the desktop must allow a non-empty client area");
		return maximum;
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
		Require(window.GetHDC() != nullptr, "the native window must retain a valid device context");
		Require(GetPixelFormat(window.GetHDC()) == 0,
			"a Vulkan window must leave the OpenGL pixel format unset");
		std::cout << "[PASS] Native window creation requires no OpenGL pixel format\n";
		Require(AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(window.GetHWND()),
			DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), "the native window must inherit PerMonitorV2");
		CheckClientExtent(window, 640, 400);
		std::cout << "[PASS] Startup client extent at DPI " << GetDpiForWindow(window.GetHWND()) << '\n';

		// DefWindowProc limits overlapped windows to the desktop tracking size.
		// A CI desktop can be smaller than the requested 1280x800 client area.
		const SIZE maximum = GetMaximumClientExtent(window.GetHWND());
		const int largeWidth = std::min(1280L, maximum.cx);
		const int largeHeight = std::min(800L, maximum.cy);
		window.ChangeWindowSize(largeWidth, largeHeight);
		window.Show(false);
		CheckClientExtent(window, largeWidth, largeHeight);
		const int smallWidth = std::min(853L, maximum.cx);
		const int smallHeight = std::min(479L, maximum.cy);
		window.ChangeWindowSize(smallWidth, smallHeight);
		window.Show(false);
		CheckClientExtent(window, smallWidth, smallHeight);
		std::cout << "[PASS] Resizing preserves exact physical client dimensions\n";

		window.ChangeWindowSize(maximum.cx + 128, maximum.cy + 128);
		window.Show(false);
		CheckClientExtent(window, maximum.cx, maximum.cy);
		std::cout << "[PASS] Renderer dimensions follow OS-limited window sizes\n";

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
