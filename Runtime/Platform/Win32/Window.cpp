#include <string>
#include <cassert>
#include "Window.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <utility>

#include <ole2.h>

#include "Input.h"
#include "Sailor.h"
#include "Tasks/Tasks.h"
#include "Tasks/Scheduler.h"
#include "Submodules/ImGuiApi.h"

#pragma comment(lib, "ole32.lib")

using namespace Sailor;
using namespace Sailor::Win32;

namespace
{
	constexpr UINT c_destroyWindowMessage = WM_APP + 0x351;
	constexpr std::wstring_view c_editorAssetDropPrefix =
		L"SailorEditor.Asset:";
	constexpr size_t c_unbracedFileIdLength = 36;
	constexpr size_t c_bracedFileIdLength = 38;
	constexpr size_t c_maxDropPayloadLength =
		c_editorAssetDropPrefix.size() + c_bracedFileIdLength;

	bool IsEditorViewportToolShortcutKey(uint32_t keyCode)
	{
		return keyCode == 'Q' ||
			keyCode == 'W' ||
			keyCode == 'E' ||
			keyCode == 'R' ||
			keyCode == 'T';
	}

	bool IsHexDigit(wchar_t value)
	{
		return (value >= L'0' && value <= L'9') ||
			(value >= L'a' && value <= L'f') ||
			(value >= L'A' && value <= L'F');
	}

	bool IsSerializedFileId(std::wstring_view fileId)
	{
		if (fileId.size() == c_bracedFileIdLength)
		{
			if (fileId.front() != L'{' || fileId.back() != L'}')
			{
				return false;
			}

			fileId = fileId.substr(1, c_unbracedFileIdLength);
		}
		else if (fileId.size() != c_unbracedFileIdLength)
		{
			return false;
		}

		for (size_t i = 0; i < fileId.size(); ++i)
		{
			const bool bHyphenPosition =
				i == 8 || i == 13 || i == 18 || i == 23;
			if ((bHyphenPosition && fileId[i] != L'-') ||
				(!bHyphenPosition && !IsHexDigit(fileId[i])))
			{
				return false;
			}
		}

		return true;
	}

	bool TryReadEditorAssetFileId(
		IDataObject* dataObject,
		std::string& outFileId)
	{
		outFileId.clear();
		if (!dataObject)
		{
			return false;
		}

		FORMATETC format{};
		format.cfFormat = CF_UNICODETEXT;
		format.dwAspect = DVASPECT_CONTENT;
		format.lindex = -1;
		format.tymed = TYMED_HGLOBAL;

		STGMEDIUM medium{};
		if (FAILED(dataObject->GetData(&format, &medium)))
		{
			return false;
		}

		bool bValid = false;
		if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal)
		{
			const SIZE_T bytes = GlobalSize(medium.hGlobal);
			const size_t availableCharacters = bytes / sizeof(wchar_t);
			const auto* text =
				static_cast<const wchar_t*>(GlobalLock(medium.hGlobal));
			if (text && availableCharacters > 0)
			{
				size_t length = 0;
				const size_t scanLimit = std::min(
					availableCharacters,
					c_maxDropPayloadLength + 1);
				while (length < scanLimit && text[length] != L'\0')
				{
					++length;
				}

				if (length < scanLimit &&
					length > c_editorAssetDropPrefix.size())
				{
					const std::wstring_view payload(text, length);
					const std::wstring_view fileId =
						payload.substr(c_editorAssetDropPrefix.size());
					if (payload.starts_with(c_editorAssetDropPrefix) &&
						IsSerializedFileId(fileId))
					{
						outFileId.reserve(fileId.size());
						for (const wchar_t value : fileId)
						{
							outFileId.push_back(
								static_cast<char>(value));
						}
						bValid = true;
					}
				}

				GlobalUnlock(medium.hGlobal);
			}
		}

		ReleaseStgMedium(&medium);
		return bValid;
	}

	class EditorViewportDropTarget final : public IDropTarget
	{
	public:
		explicit EditorViewportDropTarget(Window* window) :
			m_window(window)
		{}

		HRESULT STDMETHODCALLTYPE QueryInterface(
			REFIID interfaceId,
			void** object) override
		{
			if (!object)
			{
				return E_POINTER;
			}

			*object = nullptr;
			if (IsEqualIID(interfaceId, IID_IUnknown) ||
				IsEqualIID(interfaceId, IID_IDropTarget))
			{
				*object = static_cast<IDropTarget*>(this);
				AddRef();
				return S_OK;
			}

			return E_NOINTERFACE;
		}

		ULONG STDMETHODCALLTYPE AddRef() override
		{
			return static_cast<ULONG>(
				InterlockedIncrement(&m_referenceCount));
		}

		ULONG STDMETHODCALLTYPE Release() override
		{
			const LONG referenceCount =
				InterlockedDecrement(&m_referenceCount);
			if (referenceCount == 0)
			{
				delete this;
			}
			return static_cast<ULONG>(referenceCount);
		}

		HRESULT STDMETHODCALLTYPE DragEnter(
			IDataObject* dataObject,
			DWORD,
			POINTL,
			DWORD* effect) override
		{
			std::string fileId;
			const bool bCopyAllowed =
				effect && ((*effect & DROPEFFECT_COPY) != 0);
			m_bAcceptingDrop =
				bCopyAllowed &&
				TryReadEditorAssetFileId(dataObject, fileId);
			if (effect)
			{
				*effect &=
					m_bAcceptingDrop
						? DROPEFFECT_COPY
						: DROPEFFECT_NONE;
			}
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE DragOver(
			DWORD,
			POINTL,
			DWORD* effect) override
		{
			if (effect)
			{
				*effect &=
					m_bAcceptingDrop
						? DROPEFFECT_COPY
						: DROPEFFECT_NONE;
			}
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE DragLeave() override
		{
			m_bAcceptingDrop = false;
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE Drop(
			IDataObject* dataObject,
			DWORD,
			POINTL point,
			DWORD* effect) override
		{
			std::string fileId;
			const bool bCopyAllowed =
				effect && ((*effect & DROPEFFECT_COPY) != 0);
			bool bAccepted =
				bCopyAllowed &&
				m_bAcceptingDrop &&
				m_window &&
				TryReadEditorAssetFileId(dataObject, fileId);
			if (bAccepted)
			{
				POINT clientPoint{ point.x, point.y };
				RECT clientRect{};
				const HWND windowHandle = m_window->GetHWND();
				bAccepted =
					windowHandle &&
					ScreenToClient(windowHandle, &clientPoint) &&
					GetClientRect(windowHandle, &clientRect);
				const LONG width =
					clientRect.right - clientRect.left;
				const LONG height =
					clientRect.bottom - clientRect.top;
				bAccepted =
					bAccepted &&
					width > 0 &&
					height > 0 &&
					clientPoint.x >= clientRect.left &&
					clientPoint.x <= clientRect.right &&
					clientPoint.y >= clientRect.top &&
					clientPoint.y <= clientRect.bottom;
				if (bAccepted)
				{
					m_window->QueueEditorViewportAssetDrop(
						std::move(fileId),
						static_cast<float>(
							clientPoint.x - clientRect.left) /
							static_cast<float>(width),
						static_cast<float>(
							clientPoint.y - clientRect.top) /
							static_cast<float>(height));
				}
			}

			m_bAcceptingDrop = false;
			if (effect)
			{
				*effect &=
					bAccepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
			}
			return S_OK;
		}

	private:
		LONG m_referenceCount = 1;
		Window* m_window = nullptr;
		bool m_bAcceptingDrop = false;
	};

	void RegisterEditorViewportDropTarget(
		Window& window,
		IUnknown*& outDropTarget,
		bool& outOleInitialized)
	{
		outDropTarget = nullptr;
		outOleInitialized = false;

		const HRESULT initializeResult = OleInitialize(nullptr);
		if (FAILED(initializeResult))
		{
			SAILOR_LOG_ERROR(
				"Failed to initialize OLE for the editor viewport drop target. error=0x%08lX",
				static_cast<unsigned long>(initializeResult));
			return;
		}
		outOleInitialized = true;

		auto* dropTarget = new EditorViewportDropTarget(&window);
		const HRESULT registerResult =
			RegisterDragDrop(window.GetHWND(), dropTarget);
		if (FAILED(registerResult))
		{
			SAILOR_LOG_ERROR(
				"Failed to register the editor viewport drop target. error=0x%08lX",
				static_cast<unsigned long>(registerResult));
			dropTarget->Release();
			OleUninitialize();
			outOleInitialized = false;
			return;
		}

		outDropTarget = dropTarget;
	}

	void RevokeEditorViewportDropTarget(
		HWND window,
		IUnknown*& dropTarget,
		bool& oleInitialized)
	{
		if (dropTarget)
		{
			const HRESULT revokeResult = RevokeDragDrop(window);
			if (FAILED(revokeResult))
			{
				SAILOR_LOG_ERROR(
					"Failed to revoke the editor viewport drop target. error=0x%08lX",
					static_cast<unsigned long>(revokeResult));
			}
			dropTarget->Release();
			dropTarget = nullptr;
		}

		if (oleInitialized)
		{
			OleUninitialize();
			oleInitialized = false;
		}
	}
}

TVector<Window*> Window::g_windows;
std::mutex Window::g_windowsMutex;

Window::~Window()
{
	Destroy();
}

bool Window::IsParentWindowValid() const
{
	if (m_parentHwnd == 0)
	{
		return true;
	}

	return ::IsWindow(m_parentHwnd);
}

void Window::SetWindowPos(const RECT& rect)
{
	::SetWindowPos(m_hWnd, HWND_TOP, rect.left, rect.top,
		rect.right - rect.left, rect.bottom - rect.top,
		SWP_NOACTIVATE);
}

void Window::Show(bool bShowWindow)
{
	::ShowWindow(m_hWnd, bShowWindow ? SW_SHOW: SW_HIDE);
	m_bIsShown = bShowWindow;
}

void Window::TrackParentWindowPosition(const RECT& viewport)
{
	if (!m_parentHwnd)
	{
		return;
	}

	Utils::WindowSizeAndPosition wnd = Utils::GetWindowSizeAndPosition(m_parentHwnd);

	const bool bViewportOutdated = m_viewport.left != viewport.left || m_viewport.right != viewport.right ||
		m_viewport.bottom != viewport.bottom || m_viewport.top != viewport.top;

	const bool bWindowPosOutdated = wnd.m_windowRect.left != wnd.m_windowRect.right || wnd.m_windowRect.bottom != wnd.m_windowRect.top;

	if (bViewportOutdated || bWindowPosOutdated)
	{
		//int32_t gapWidth = (wnd.m_width - wnd.m_clientWidth) / 2 + 1;
		//int32_t gapHeight = (wnd.m_height - wnd.m_clientHeight) / 2 + 1;

		const int32_t gapWidth = 10;
		const int32_t gapHeight = 2;

		const int32_t newX = wnd.m_xPos + gapWidth + viewport.left;
		const int32_t newY = wnd.m_yPos + gapHeight + viewport.top;

		int32_t newWidth = viewport.right - viewport.left - gapWidth / 2;
		int32_t newHeight = viewport.bottom - viewport.top - gapHeight * 2;

		newWidth = std::min(newWidth, wnd.m_width - (int32_t)viewport.left);
		newHeight = std::min(newHeight, wnd.m_height - (int32_t)viewport.top);

		if (newWidth > 0 && newHeight > 0)
		{
			::SetWindowPos(m_hWnd, m_parentHwnd,
				newX, newY,
				newWidth, newHeight,
				SWP_NOACTIVATE);
		}

		m_viewport = viewport;
	}
}

bool Window::Create(LPCSTR title, LPCSTR className, int32_t inWidth, int32_t inHeight, bool inbIsFullScreen, bool bIsVsyncRequested, HWND parentHwnd)
{
	m_parentHwnd = parentHwnd;
	m_windowClassName = className;

	WNDCLASSEX            wcx{};
	RECT                  rect{};
	DWORD                 style{}, exStyle{};
	int32_t               x{}, y{};

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

	m_windowClassAtom = RegisterClassEx(&wcx);
	if (m_windowClassAtom == 0)
	{
		char message[MAXCHAR];
		sprintf_s(message, "RegisterClassEx fail (%d)", GetLastError());
		return false;
	}

	style = m_parentHwnd ? WS_POPUP : WS_OVERLAPPEDWINDOW;
	exStyle = m_parentHwnd ? 0 : WS_EX_APPWINDOW;

	x = (GetSystemMetrics(SM_CXSCREEN) - inWidth) / 2;
	y = (GetSystemMetrics(SM_CYSCREEN) - inHeight) / 2;

	rect.left = x;
	rect.right = x + inWidth;
	rect.top = y;
	rect.bottom = y + inHeight;

	if (m_parentHwnd == nullptr)
	{
		AdjustWindowRectEx(&rect, style, FALSE, exStyle);
	}

	// Create window
	m_hWnd = CreateWindowEx(exStyle,
		m_windowClassName.c_str(),
		title,
		style,
		rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
		parentHwnd,
		NULL,
		m_hInstance,
		NULL);

	if (!m_hWnd)
	{
		char message[MAXCHAR];
		sprintf_s(message, "CreateWindowEx fail (%d)", GetLastError());
		Destroy();
		return false;
	}

	m_hDC = GetDC(m_hWnd);

	if (!m_hDC)
	{
		char message[MAXCHAR];
		sprintf_s(message, "GetDC fail (%d)", GetLastError());
		Destroy();
		return false;
	}

	PIXELFORMATDESCRIPTOR pfd;
	int32_t format;

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
		Destroy();
		return false;
	}

	{
		const std::lock_guard<std::mutex> lock(g_windowsMutex);
		g_windows.Add(this);
	}

	m_bIsVsyncRequested = bIsVsyncRequested;
	m_width = inWidth;
	m_height = inHeight;
	m_bIsFullscreen = inbIsFullScreen;

	ChangeWindowSize(m_width, m_height, m_bIsFullscreen);

	if (m_parentHwnd &&
		m_windowClassName.starts_with("SailorEditor"))
	{
		RegisterEditorViewportDropTarget(
			*this,
			m_editorViewportDropTarget,
			m_bEditorViewportDropOleInitialized);
	}

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

	if (bInIsFullScreen && !m_bIsFullscreen)
	{
		ChangeDisplaySettings(NULL, CDS_RESET);
		ShowCursor(TRUE);
	}

	m_bIsFullscreen = bInIsFullScreen;

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

	if (m_bIsFullscreen)
	{
		ShowCursor(FALSE);
		style = WS_POPUP;
		exStyle = WS_EX_APPWINDOW | WS_EX_TOPMOST;

		x = y = 0;
	}
	else
	{
		style = m_parentHwnd ? WS_POPUP : WS_OVERLAPPEDWINDOW;
		exStyle = m_parentHwnd ? 0 : WS_EX_APPWINDOW;

		x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
		y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
	}

	rect.left = x;
	rect.right = x + width;
	rect.top = y;
	rect.bottom = y + height;

	if (m_parentHwnd == 0)
	{
		AdjustWindowRectEx(&rect, style, FALSE, exStyle);
	}

	SetWindowLong(m_hWnd, GWL_STYLE, style);
	SetWindowLong(m_hWnd, GWL_EXSTYLE, exStyle);

	// Refresh window position
	::SetWindowPos(m_hWnd, HWND_TOP, rect.left, rect.top,
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

	SetCursorPos(x + width / 2, y + height / 2);
}

void Sailor::Win32::Window::ProcessWin32Msgs()
{
	SAILOR_PROFILE_FUNCTION();

	MSG msg;
	for (int i = 0; ; i++)
	{
		Window* pWindow = nullptr;
		HWND hWnd = nullptr;
		{
			const std::lock_guard<std::mutex> lock(g_windowsMutex);
			if (i >= g_windows.Num())
			{
				break;
			}
			pWindow = g_windows[i];
			hWnd = pWindow->m_hWnd;
		}

		while (PeekMessage(&msg, hWnd, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
			{
				pWindow->SetRunning(false);
				break;
			}
			DispatchMessage(&msg);
		}
	}
}

void Window::ProcessSystemMessages()
{
	ProcessWin32Msgs();
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
	HWND hWnd = nullptr;
	{
		const std::lock_guard<std::mutex> lock(g_windowsMutex);
		hWnd = m_hWnd;
	}

	if (hWnd &&
		::GetWindowThreadProcessId(hWnd, nullptr) !=
			::GetCurrentThreadId())
	{
		// The Window object cannot be released until its HWND has been
		// destroyed. A timeout would let the destructor return while
		// WindowProc still retains this pointer in g_windows.
		const LRESULT result = ::SendMessage(
			hWnd,
			c_destroyWindowMessage,
			0,
			0);
		if (result != TRUE)
		{
			const std::lock_guard<std::mutex> lock(g_windowsMutex);
			// The owner may have destroyed the HWND between the thread check
			// and SendMessage. Never leave WindowProc with a soon-to-be-freed
			// Window pointer even if native resources can no longer be
			// reclaimed safely from this thread.
			if (m_hWnd == hWnd)
			{
				SAILOR_LOG_ERROR(
					"Win32 window owner did not acknowledge destruction. error=%lu",
					static_cast<unsigned long>(GetLastError()));
				m_hWnd = nullptr;
				m_hDC = nullptr;
				g_windows.Remove(this);
			}
		}
		return;
	}

	check(!hWnd || ::GetWindowThreadProcessId(hWnd, nullptr) == ::GetCurrentThreadId());

	if (m_editorViewportDropTarget ||
		m_bEditorViewportDropOleInitialized)
	{
		RevokeEditorViewportDropTarget(
			hWnd,
			m_editorViewportDropTarget,
			m_bEditorViewportDropOleInitialized);
	}

	HDC hDC = nullptr;
	HINSTANCE hInstance = nullptr;
	ATOM windowClassAtom = 0;
	const bool bWasFullscreen = m_bIsFullscreen;

	{
		const std::lock_guard<std::mutex> lock(g_windowsMutex);
		hWnd = m_hWnd;
		hDC = m_hDC;
		hInstance = m_hInstance;
		windowClassAtom = m_windowClassAtom;
		m_hWnd = nullptr;
		m_hDC = nullptr;
		m_hInstance = nullptr;
		m_parentHwnd = nullptr;
		m_windowClassAtom = 0;
		g_windows.Remove(this);
	}

	m_bIsShown = false;
	m_bIsFullscreen = false;
	m_bIsActive = false;
	m_bIsRunning = false;
	m_bIsIconic = false;
	m_bIsResizing = false;
	m_bIsVsyncRequested = false;
	m_width = 0;
	m_height = 0;
	m_renderArea = {};
	m_viewport = {};
	m_windowClassName.clear();
	{
		const std::lock_guard<std::mutex> lock(
			m_editorViewportAssetDropMutex);
		m_pendingEditorViewportAssetDrop.reset();
	}
	{
		const std::lock_guard<std::mutex> lock(
			m_editorViewportToolShortcutMutex);
		m_pendingEditorViewportToolShortcuts.Clear();
	}

	if (bWasFullscreen)
	{
		ChangeDisplaySettings(NULL, CDS_RESET);
		ShowCursor(TRUE);
	}

	if (hWnd && hDC && ReleaseDC(hWnd, hDC) == 0)
	{
		SAILOR_LOG_ERROR("Failed to release window device context. error=%lu", static_cast<unsigned long>(GetLastError()));
	}

	bool bWindowDestroyed = true;
	if (hWnd && !DestroyWindow(hWnd))
	{
		bWindowDestroyed = false;
		SAILOR_LOG_ERROR("Failed to destroy Win32 window. error=%lu", static_cast<unsigned long>(GetLastError()));
	}

	if (windowClassAtom != 0 && hInstance && bWindowDestroyed && !UnregisterClass(MAKEINTATOM(windowClassAtom), hInstance))
	{
		SAILOR_LOG_ERROR("Failed to unregister Win32 window class. error=%lu", static_cast<unsigned long>(GetLastError()));
	}
}

void Window::QueueEditorViewportAssetDrop(
	std::string fileId,
	float normalizedX,
	float normalizedY)
{
	const std::lock_guard<std::mutex> lock(
		m_editorViewportAssetDropMutex);
	m_pendingEditorViewportAssetDrop = EditorViewportAssetDrop{
		std::move(fileId),
		normalizedX,
		normalizedY
	};
}

bool Window::PullEditorViewportAssetDrop(
	std::string& outFileId,
	float& outNormalizedX,
	float& outNormalizedY)
{
	outFileId.clear();
	outNormalizedX = 0.0f;
	outNormalizedY = 0.0f;

	const std::lock_guard<std::mutex> lock(
		m_editorViewportAssetDropMutex);
	if (!m_pendingEditorViewportAssetDrop)
	{
		return false;
	}

	outFileId = std::move(
		m_pendingEditorViewportAssetDrop->m_fileId);
	outNormalizedX =
		m_pendingEditorViewportAssetDrop->m_normalizedX;
	outNormalizedY =
		m_pendingEditorViewportAssetDrop->m_normalizedY;
	m_pendingEditorViewportAssetDrop.reset();
	return true;
}

void Window::QueueEditorViewportToolShortcut(uint32_t keyCode)
{
	if (!IsEditorViewportToolShortcutKey(keyCode))
	{
		return;
	}

	const std::lock_guard<std::mutex> lock(
		m_editorViewportToolShortcutMutex);
	m_pendingEditorViewportToolShortcuts.Add(keyCode);
}

bool Window::PullEditorViewportToolShortcut(uint32_t& outKeyCode)
{
	outKeyCode = 0;
	const std::lock_guard<std::mutex> lock(
		m_editorViewportToolShortcutMutex);
	if (m_pendingEditorViewportToolShortcuts.IsEmpty())
	{
		return false;
	}

	outKeyCode = m_pendingEditorViewportToolShortcuts[0];
	m_pendingEditorViewportToolShortcuts.RemoveAt(0);
	return true;
}

bool Window::IsIconic() const
{
	return ::IsIconic(m_hWnd);
}

LRESULT CALLBACK Sailor::Win32::WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	Window* pWindow = nullptr;
	{
		const std::lock_guard<std::mutex> lock(Window::g_windowsMutex);
		auto windowIndex = Window::g_windows.FindIf(
			[hWnd](Window* pCandidate)
			{
				return pCandidate->m_hWnd == hWnd;
			});
		if (windowIndex != -1)
		{
			pWindow = Window::g_windows[windowIndex];
		}
	}
	if (!pWindow)
	{
		return DefWindowProc(hWnd, msg, wParam, lParam);
	}

	if (auto* imGui = App::GetSubmodule<ImGuiApi>())
	{
		imGui->HandleWin32(hWnd, msg, wParam, lParam);
	}

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
		GlobalInput::SetCursorPosition((int32_t)LOWORD(lParam), (int32_t)HIWORD(lParam));

		if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)
			GlobalInput::SetMouseButtonState(0, msg == WM_LBUTTONDOWN ? KeyState::Pressed : KeyState::Up);

		if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP)
			GlobalInput::SetMouseButtonState(1, msg == WM_RBUTTONDOWN ? KeyState::Pressed : KeyState::Up);

		if (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP)
			GlobalInput::SetMouseButtonState(2, msg == WM_MBUTTONDOWN ? KeyState::Pressed : KeyState::Up);

		return FALSE;
	}

	case WM_MOUSEMOVE:
	{
		GlobalInput::SetCursorPosition((int32_t)LOWORD(lParam), (int32_t)HIWORD(lParam));
		return FALSE;
	}

	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
	{
		if (wParam < 256 && (lParam & 0x40000000) == 0)
		{
			const uint32_t keyCode = static_cast<uint32_t>(wParam);
			GlobalInput::SetKeyState(keyCode, KeyState::Pressed);
			if (pWindow->m_parentHwnd &&
				pWindow->m_windowClassName.starts_with("SailorEditor") &&
				IsEditorViewportToolShortcutKey(keyCode))
			{
				pWindow->QueueEditorViewportToolShortcut(keyCode);
			}
		}

		return FALSE;
	}
	case WM_KEYUP:
	case WM_SYSKEYUP:
	{
		if (wParam < 256)
		{
			GlobalInput::SetKeyState((uint32_t)wParam, KeyState::Up);
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
	case c_destroyWindowMessage:
		pWindow->Destroy();
		return TRUE;

	case WM_NCDESTROY:
	{
		if (pWindow->m_editorViewportDropTarget ||
			pWindow->m_bEditorViewportDropOleInitialized)
		{
			RevokeEditorViewportDropTarget(
				hWnd,
				pWindow->m_editorViewportDropTarget,
				pWindow->m_bEditorViewportDropOleInitialized);
		}
		{
			const std::lock_guard<std::mutex> assetDropLock(
				pWindow->m_editorViewportAssetDropMutex);
			pWindow->m_pendingEditorViewportAssetDrop.reset();
		}
		{
			const std::lock_guard<std::mutex> shortcutLock(
				pWindow->m_editorViewportToolShortcutMutex);
			pWindow->m_pendingEditorViewportToolShortcuts.Clear();
		}

		const std::lock_guard<std::mutex> lock(Window::g_windowsMutex);
		pWindow->m_hWnd = nullptr;
		pWindow->m_hDC = nullptr;
		Window::g_windows.Remove(pWindow);
	}
		pWindow->m_bIsShown = false;
		pWindow->m_bIsActive = false;
		pWindow->m_bIsRunning = false;
		return DefWindowProc(hWnd, msg, wParam, lParam);

	case WM_SYSCOMMAND:
	{
		switch (wParam & 0xFFF0)
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
