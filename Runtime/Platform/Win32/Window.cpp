#include <string>
#include <cassert>
#include "Window.h"

#include <algorithm>

#include "Input.h"
#include "Sailor.h"
#include "Tasks/Tasks.h"
#include "Tasks/Scheduler.h"
#include "Submodules/ImGuiApi.h"

using namespace Sailor;
using namespace Sailor::Win32;

TVector<Window*> Window::g_windows;

bool Window::Create(LPCSTR title, LPCSTR className, int32_t inWidth, int32_t inHeight, bool inbIsFullScreen, bool bIsVsyncRequested)
{
	m_windowClassName = className;
	m_bIsVsyncRequested = bIsVsyncRequested;
	m_width = inWidth;
	m_height = inHeight;
	m_bIsFullscreen = inbIsFullScreen;

	WNDCLASSEX            wcx;
	PIXELFORMATDESCRIPTOR pfd;
	RECT                  rect;
	DWORD                 style, exStyle;
	int                   x, y, format;

	m_hInstance = static_cast<HINSTANCE>(GetModuleHandle(NULL));

	// Window class registration
	memset(&wcx, 0, sizeof(wcx));
	wcx.cbSize = sizeof(wcx);
	wcx.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
	wcx.lpfnWndProc = reinterpret_cast<WNDPROC>(WindowProc);
	wcx.hInstance = m_hInstance;
	wcx.lpszClassName = className;
	wcx.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	wcx.hCursor = LoadCursor(NULL, IDC_ARROW);

	if (!RegisterClassEx(&wcx))
	{
		char message[MAXCHAR];
		sprintf_s(message, "RegisterClassEx fail (%d)", GetLastError());
		return false;
	}

	// Window styles
	style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_OVERLAPPED;
	exStyle = WS_EX_APPWINDOW;

	// Centrate window
	x = (GetSystemMetrics(SM_CXSCREEN) - m_width) / 2;
	y = (GetSystemMetrics(SM_CYSCREEN) - m_height) / 2;

	rect.left = x;
	rect.right = x + m_width;
	rect.top = y;
	rect.bottom = y + m_height;

	// Setup window size with styles
	AdjustWindowRectEx(&rect, style, FALSE, exStyle);

	// Create window
	m_hWnd = CreateWindowEx(exStyle, m_windowClassName.c_str(), title, style, rect.left, rect.top,
		rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, m_hInstance, NULL);

	if (!m_hWnd)
	{
		char message[MAXCHAR];
		sprintf_s(message, "CreateWindowEx fail (%d)", GetLastError());
		return false;
	}

	g_windows.Add(this);

	// ÔGet window descriptor
	m_hDC = GetDC(m_hWnd);

	if (!m_hDC)
	{
		char message[MAXCHAR];
		sprintf_s(message, "GetDC fail (%d)", GetLastError());
		return false;
	}

	// Pixel format description
	memset(&pfd, 0, sizeof(pfd));
	pfd.nSize = sizeof(pfd);
	pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 32;
	pfd.cDepthBits = 24;

	// Get pixel format for format which is described above
	format = ChoosePixelFormat(m_hDC, &pfd);
	if (!format || !SetPixelFormat(m_hDC, format, &pfd))
	{
		char message[MAXCHAR];
		sprintf_s(message, "Setting pixel format fail (%d)", GetLastError());
		return false;
	}

	// TODO: Init Vulkan

	// Set up window size
	ChangeWindowSize(m_width, m_height, m_bIsFullscreen);

	SAILOR_LOG("Window created");
	return true;
}

void Window::ChangeWindowSize(int32_t width, int32_t height, bool bInIsFullScreen)
{
	RECT    rect;
	DWORD   style, exStyle;
	DEVMODE devMode;
	LONG    result;
	int     x, y;

	// If we change from fullscreen mode
	if (bInIsFullScreen && !m_bIsFullscreen)
	{
		ChangeDisplaySettings(NULL, CDS_RESET);
		ShowCursor(TRUE);
	}

	m_bIsFullscreen = bInIsFullScreen;

	// Fullscreen mode
	if (m_bIsFullscreen)
	{
		memset(&devMode, 0, sizeof(devMode));
		devMode.dmSize = sizeof(devMode);
		devMode.dmPelsWidth = width;
		devMode.dmPelsHeight = height;
		devMode.dmBitsPerPel = GetDeviceCaps(m_hDC, BITSPIXEL);
		devMode.dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;

		// Try to set fullscreen mode
		result = ChangeDisplaySettings(&devMode, CDS_FULLSCREEN);
		if (result != DISP_CHANGE_SUCCESSFUL)
		{
			char message[MAXCHAR];
			sprintf_s(message, "ChangeDisplaySettings fail %dx%d (%d)", width, height, result);
			m_bIsFullscreen = false;
		}
	}

	// If fullscreen mode setuped succesfully
	if (m_bIsFullscreen)
	{
		ShowCursor(FALSE);
		style = WS_POPUP;
		exStyle = WS_EX_APPWINDOW | WS_EX_TOPMOST;

		x = y = 0;
	}
	else
	{ //Window
		style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_OVERLAPPEDWINDOW;
		exStyle = WS_EX_APPWINDOW;

		// Centralize window
		x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
		y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
	}

	rect.left = x;
	rect.right = x + width;
	rect.top = y;
	rect.bottom = y + height;

	//Setup styles
	AdjustWindowRectEx(&rect, style, FALSE, exStyle);

	SetWindowLong(m_hWnd, GWL_STYLE, style);
	SetWindowLong(m_hWnd, GWL_EXSTYLE, exStyle);

	// Refresh window position
	SetWindowPos(m_hWnd, HWND_TOP, rect.left, rect.top,
		rect.right - rect.left, rect.bottom - rect.top,
		SWP_FRAMECHANGED);

	// Show
	ShowWindow(m_hWnd, SW_SHOW);
	SetForegroundWindow(m_hWnd);
	SetFocus(m_hWnd);
	UpdateWindow(m_hWnd);

	// Get sizes of the window
	GetClientRect(m_hWnd, &rect);
	width = rect.right - rect.left;
	height = rect.bottom - rect.top;

	// Centralize cursor in the center of the screen
	SetCursorPos(x + width / 2, y + height / 2);
}

void Sailor::Win32::Window::ProcessWin32Msgs()
{
	SAILOR_PROFILE_FUNCTION()

	MSG msg;
	for (int i = 0; i < g_windows.Num(); i++)
	{
		while (PeekMessage(&msg, g_windows[i]->GetHWND(), 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
			{
				g_windows[i]->SetRunning(false);
				break;
			}
			DispatchMessage(&msg);
		}
	}
}

glm::ivec2 Window::GetCenterPointScreen() const
{
	glm::ivec2 centerClient = GetCenterPointClient();
	POINT point{ centerClient.x, centerClient.y };
	::ClientToScreen(m_hWnd, &point);

	return ivec2(point.x, point.y);
}

glm::ivec2 Window::GetCenterPointClient() const
{
	return ivec2(m_width / 2, m_height / 2);
}

void Window::RecalculateWindowSize()
{
	if (IsIconic())
	{
		m_width = 0;
		m_height = 0;
		return;
	}

	RECT rect;

	if (GetWindowRect(m_hWnd, &rect))
	{
		m_width = rect.right - rect.left;
		m_height = rect.bottom - rect.top;
	}
}

void Window::Destroy()
{
	// Restore window size
	if (m_bIsFullscreen)
	{
		ChangeDisplaySettings(NULL, CDS_RESET);
		ShowCursor(TRUE);
	}

	// Release window context
	if (m_hDC)
	{
		ReleaseDC(m_hWnd, m_hDC);
	}

	// Destroy window
	if (m_hWnd)
	{
		DestroyWindow(m_hWnd);
	}

	// Release window class
	if (m_hInstance)
	{
		UnregisterClass(m_windowClassName.c_str(), m_hInstance);
	}

	g_windows.Remove(this);
}

bool Window::IsIconic() const
{
	return ::IsIconic(m_hWnd);
}

LRESULT CALLBACK Sailor::Win32::WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	auto windowIndex = Window::g_windows.FindIf([hWnd](Window* pWindow) { return pWindow->GetHWND() == hWnd; });

	Window* pWindow = nullptr;
	if (windowIndex != -1)
	{
		pWindow = Window::g_windows[windowIndex];
	}

	App::GetSubmodule<ImGuiApi>()->HandleWin32(hWnd, msg, wParam, lParam);

	switch (msg)
	{
	case WM_SIZE:
	{
		pWindow->SetIsIconic(wParam == SIZE_MINIMIZED);
		//pWindow->RecalculateWindowSize();
		pWindow->m_width = LOWORD(lParam);
		pWindow->m_height = HIWORD(lParam);

		return FALSE;
	}
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
	{
		GlobalInput::m_rawState.m_cursorPosition[0] = (int32_t)LOWORD(lParam);
		GlobalInput::m_rawState.m_cursorPosition[1] = (int32_t)HIWORD(lParam);

		if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)
			GlobalInput::m_rawState.m_keyboard[0] = (msg == WM_LBUTTONDOWN ? KeyState::Pressed : KeyState::Up);

		if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP)
			GlobalInput::m_rawState.m_keyboard[1] = (msg == WM_RBUTTONDOWN ? KeyState::Pressed : KeyState::Up);

		if (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP)
			GlobalInput::m_rawState.m_keyboard[2] = (msg == WM_MBUTTONDOWN ? KeyState::Pressed : KeyState::Up);

		return FALSE;
	}

	case WM_MOUSEMOVE:
	{
		GlobalInput::m_rawState.m_cursorPosition[0] = (int32_t)LOWORD(lParam);
		GlobalInput::m_rawState.m_cursorPosition[1] = (int32_t)HIWORD(lParam);
		return FALSE;
	}

	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
	{
		if (wParam < 256 && (lParam & 0x40000000) == 0)
		{
			GlobalInput::m_rawState.m_keyboard[wParam] = KeyState::Pressed;
		}

		return FALSE;
	}
	case WM_KEYUP:
	case WM_SYSKEYUP:
	{
		if (wParam < 256)
		{
			GlobalInput::m_rawState.m_keyboard[wParam] = KeyState::Up;
		}

		return FALSE;
	}

	case WM_SETFOCUS:
	case WM_KILLFOCUS:
		pWindow->SetActive(msg == WM_SETFOCUS);
		return FALSE;

	case WM_ACTIVATE:
		pWindow->SetActive(LOWORD(wParam) == WA_INACTIVE);
		return FALSE;

	case WM_CLOSE:
	{
		pWindow->SetActive(false);
		pWindow->SetRunning(false);

		PostQuitMessage(0);
		return FALSE;
	}
	case WM_SYSCOMMAND:
	{    switch (wParam & 0xFFF0)
	{
	case SC_SCREENSAVE:
	case SC_MONITORPOWER:
		if (pWindow->IsFullscreen())
			return FALSE;
		break;
	case SC_KEYMENU:
		return FALSE;
	}
	break;
	}
	case WM_ERASEBKGND:
		return FALSE;
	}

	return DefWindowProc(hWnd, msg, wParam, lParam);
}
