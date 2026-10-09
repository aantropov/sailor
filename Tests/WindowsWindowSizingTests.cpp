#include "Platform/Win32/Window.h"
#include "Platform/Win32/Input.h"
#include "Support/ScopeExit.h"
#include <windows.h>
#include <algorithm>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace
{
	void Require(bool condition, std::string_view message)
	{
		if (!condition)
		{
			throw std::runtime_error(std::string(message));
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

	void CheckInputOwner(Sailor::Win32::Window& window)
	{
		using namespace Sailor::Win32;
		GlobalInput::ProcessPendingEvents(false);
		GlobalInput::Reset();
		SendMessage(window.GetHWND(), WM_KEYDOWN, 'W', 1);
		SendMessage(window.GetHWND(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(static_cast<WORD>(-9), 41));
		Require(!GlobalInput::GetInputState().IsKeyDown('W'), "WndProc must not modify frame input on the UI thread");
		InputState pressed;
		std::thread frameOwner([&]
		{
			// An embedded engine must not pump another thread's HWND or update its capture.
			window.ProcessSystemMessages();
			GlobalInput::ProcessPendingEvents(true);
			pressed = GlobalInput::GetInputState();
		});
		frameOwner.join();
		Require(pressed.IsKeyPressed('W') && pressed.IsButtonClick(VK_LBUTTON) &&
			pressed.GetButtonPressCursorPos(VK_LBUTTON) == glm::ivec2(-9, 41),
			"the frame owner must receive native keys and signed button coordinates");
		SendMessage(window.GetHWND(), WM_KILLFOCUS, 0, 0);
		Require(GlobalInput::GetInputState().IsKeyDown('W'), "UI focus loss must wait for the frame owner");
		std::thread releaseOwner([] { GlobalInput::ProcessPendingEvents(true); });
		releaseOwner.join();
		Require(!GlobalInput::GetInputState().IsKeyDown('W') && !GlobalInput::GetInputState().IsButtonDown(VK_LBUTTON),
			"focus loss must clear input even without a key-up delivered to this window");
		SendMessage(window.GetHWND(), WM_SETFOCUS, 0, 0);
		GlobalInput::ProcessPendingEvents(true);
		Require(!GlobalInput::GetInputState().IsKeyDown('W'), "regaining focus must not resurrect a held key");
	}

	void CheckModifierSides(Sailor::Win32::Window& window)
	{
		using namespace Sailor::Win32;
		struct Modifier { uint32_t aggregate; uint32_t key[2]; uint32_t scan[2]; bool bRightExtended; };
		const Modifier modifiers[] = {
			{ VK_SHIFT, { VK_LSHIFT, VK_RSHIFT }, { 0x2A, 0x36 }, false },
			{ VK_CONTROL, { VK_LCONTROL, VK_RCONTROL }, { 0x1D, 0x1D }, true },
			{ VK_MENU, { VK_LMENU, VK_RMENU }, { 0x38, 0x38 }, true },
			{ 0, { VK_LWIN, VK_RWIN }, { 0x5B, 0x5C }, true }
		};
		auto drain = []
		{
			InputState result;
			std::thread owner([&]
			{
				GlobalInput::ProcessPendingEvents(true);
				result = GlobalInput::GetInputState();
			});
			owner.join();
			return result;
		};
		for (const auto& modifier : modifiers)
		{
			auto dispatch = [&](uint32_t side, bool bPressed)
			{
				const bool bIsSystem = modifier.aggregate == VK_MENU;
				const auto key = modifier.aggregate ? modifier.aggregate : modifier.key[side];
				LPARAM flags = 1 | (static_cast<LPARAM>(modifier.scan[side]) << 16);
				if ((side && modifier.bRightExtended) || modifier.aggregate == 0) flags |= LPARAM(1) << 24;
				if (!bPressed) flags |= (LPARAM(1) << 30) | (LPARAM(1) << 31);
				SendMessage(window.GetHWND(), bPressed ? (bIsSystem ? WM_SYSKEYDOWN : WM_KEYDOWN) :
					(bIsSystem ? WM_SYSKEYUP : WM_KEYUP), key, flags);
			};
			for (uint32_t first : { 0u, 1u })
			{
				const uint32_t second = 1u - first;
				GlobalInput::ProcessPendingEvents(false);
				GlobalInput::Reset();
				dispatch(first, true);
				Require(!GlobalInput::GetInputState().IsKeyDown(modifier.key[first]), "WndProc must enqueue physical modifiers");
				auto state = drain();
				Require(state.IsKeyDown(modifier.key[first]) && !state.IsKeyDown(modifier.key[second]) &&
					(!modifier.aggregate || state.IsKeyDown(modifier.aggregate)), "scan codes must preserve the physical modifier side");
				dispatch(second, true);
				dispatch(first, false);
				state = drain();
				Require(!state.IsKeyDown(modifier.key[first]) && state.IsKeyDown(modifier.key[second]) &&
					(!modifier.aggregate || state.IsKeyDown(modifier.aggregate)), "one native release must preserve the held sibling");
				dispatch(second, false);
				state = drain();
				Require(!state.IsKeyDown(modifier.key[0]) && !state.IsKeyDown(modifier.key[1]) &&
					(!modifier.aggregate || !state.IsKeyDown(modifier.aggregate)), "the last native release must clear the modifier");
			}
		}
	}

	struct PumpRetirement
	{
		Sailor::Win32::Window* m_window = nullptr;
		WNDPROC m_previousProc = nullptr;
		DWORD m_callbackThread = 0;
		bool m_bIsPageProtected = false;
	};

	constexpr UINT RetireWindowMessage = WM_APP + 17;

	LRESULT CALLBACK RetiringWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		auto& retirement = *reinterpret_cast<PumpRetirement*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
		if (message == RetireWindowMessage)
		{
			retirement.m_callbackThread = GetCurrentThreadId();
			auto* window = std::exchange(retirement.m_window, nullptr);
			window->~Window();
			// Any later access by the message pump must fail, not depend on heap reuse.
			DWORD previousProtection = 0;
			retirement.m_bIsPageProtected = VirtualProtect(window, sizeof(*window), PAGE_NOACCESS, &previousProtection) != FALSE;
			return 0;
		}
		return CallWindowProc(retirement.m_previousProc, hwnd, message, wParam, lParam);
	}

	void CheckMessagePumpRetirement()
	{
		using namespace Sailor::Win32;
		const HWND hostWindow = CreateWindowExW(0, L"STATIC", L"Host message queue", 0,
			0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
		Require(hostWindow != nullptr, "the host must have its own message-only window");
		Sailor::Tests::ScopeExit closeHost([&]() { DestroyWindow(hostWindow); });
		for (uint32_t iteration = 0; iteration < 6; ++iteration)
		{
			const bool bUseSentMessage = iteration % 2 != 0;
			GlobalInput::ProcessPendingEvents(false);
			GlobalInput::Reset();
			void* storage = VirtualAlloc(nullptr, sizeof(Window), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
			Require(storage != nullptr, "the retiring window must have separately protected storage");
			PumpRetirement retirement{ new (storage) Window() };
			Sailor::Tests::ScopeExit release([&]()
			{
				if (retirement.m_window)
				{
					retirement.m_window->~Window();
				}
				VirtualFree(storage, 0, MEM_RELEASE);
			});
			Require(retirement.m_window->Create("Retiring window", "SailorPumpRetirement", 160, 120),
				"the retiring native window must be created");
			retirement.m_window->Show(false);
			const auto retiredHandle = retirement.m_window->GetHWND();
			SetWindowLongPtr(retiredHandle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&retirement));
			retirement.m_previousProc = reinterpret_cast<WNDPROC>(SetWindowLongPtr(
				retiredHandle, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(RetiringWindowProc)));
			Require(retirement.m_previousProc != nullptr, "the retirement callback must replace the native window procedure");

			Window survivor;
			Require(survivor.Create("Surviving window", "SailorPumpSurvivor", 160, 120),
				"a second native window must survive registry removal");
			survivor.Show(false);
			if (!bUseSentMessage)
			{
				Require(PostMessage(retiredHandle, RetireWindowMessage, 0, 0), "the retirement message must be queued");
			}
			Require(PostMessage(survivor.GetHWND(), WM_KEYDOWN, 'P', 1), "the surviving window's input must be queued");
			bool bMessageSent = true;
			std::thread engineThread([&]()
			{
				Window::ProcessWin32Msgs();
				if (bUseSentMessage)
				{
					bMessageSent = SendNotifyMessage(retiredHandle, RetireWindowMessage, 0, 0) != FALSE;
				}
			});
			engineThread.join();
			Require(bMessageSent && retirement.m_window != nullptr && retirement.m_callbackThread == 0,
				"the engine thread must not dispatch windows owned by the UI thread");

			Window::ProcessWin32Msgs();
			Require(retirement.m_window == nullptr && retirement.m_bIsPageProtected &&
				retirement.m_callbackThread == GetCurrentThreadId() && !IsWindow(retiredHandle),
				"owner-thread dispatch must survive destruction and protected release of its current Window");
			GlobalInput::ProcessPendingEvents(true);
			Require(GlobalInput::GetInputState().IsKeyDown('P'),
				"removing one window must not skip the next window's queued input");
			GlobalInput::Reset();
			survivor.SetRunning(true);
			Require(PostMessage(hostWindow, WM_APP + 18, 0, 0), "the host's unrelated message must be queued");
			PostQuitMessage(0);
			Window::ProcessWin32Msgs();
			MSG hostMessage{};
			Require(PeekMessage(&hostMessage, hostWindow, WM_APP + 18, WM_APP + 18, PM_REMOVE) &&
				hostMessage.hwnd == hostWindow && hostMessage.message == WM_APP + 18,
				"the engine pump must leave unrelated host messages for the host loop");
			// PostQuitMessage waits for the host to drain its own queued work.
			Window::ProcessWin32Msgs();
			Require(!survivor.IsRunning(), "thread quit must stop the surviving native window after the host drains its messages");
		}
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
		const int largeWidth = std::min<LONG>(1280, maximum.cx);
		const int largeHeight = std::min<LONG>(800, maximum.cy);
		window.ChangeWindowSize(largeWidth, largeHeight);
		window.Show(false);
		CheckClientExtent(window, largeWidth, largeHeight);
		const int smallWidth = std::min<LONG>(853, maximum.cx);
		const int smallHeight = std::min<LONG>(479, maximum.cy);
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
		CheckInputOwner(window);
		std::cout << "[PASS] Native input delivery and focus release on a separate frame owner\n";
		CheckModifierSides(window);
		std::cout << "[PASS] Left/right native modifiers and aggregate release\n";
		window.Destroy();
		CheckMessagePumpRetirement();
		std::cout << "[PASS] Owner-thread pump, reentrant window retirement and registry reuse\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "[FAIL] " << error.what() << '\n';
		return 1;
	}
	return 0;
}
