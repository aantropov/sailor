#if defined(__APPLE__)

#include "Platform/Win32/Window.h"
#include "Platform/Win32/Input.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/UniquePtr.hpp"

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <IOKit/hidsystem/IOLLEvent.h>
#import <QuartzCore/CAMetalLayer.h>
#import <dispatch/dispatch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

using Sailor::Win32::Window;

namespace
{
	constexpr int32_t TestWidth = 321;
	constexpr int32_t TestHeight = 177;
	constexpr const char* WorkerTitle = "Sailor | Шторм — Storm";

	void Require(bool condition, std::string_view message)
	{
		if (!condition)
		{
			throw std::runtime_error(std::string(message));
		}
	}

	template<typename Predicate>
	bool PumpMainRunLoopUntil(Predicate&& predicate, std::chrono::steady_clock::duration timeout)
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (!predicate() && std::chrono::steady_clock::now() < deadline)
		{
			@autoreleasepool
			{
				[[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
			}
		}

		return predicate();
	}

	void DrainMainQueue()
	{
		auto completed = Sailor::TSharedPtr<std::atomic_bool>::Make(false);
		dispatch_async(dispatch_get_main_queue(), ^
		{
			completed->store(true, std::memory_order_release);
		});
		Require(PumpMainRunLoopUntil(
			[completed]() { return completed->load(std::memory_order_acquire); },
			std::chrono::seconds(2)), "main queue should complete pending window updates");
	}

	struct BackgroundWindowUpdateState
	{
		Sailor::TSharedPtr<Window> m_window;
		std::mutex m_mutex;
		std::condition_variable m_completedCondition;
		std::exception_ptr m_failure;
		bool m_completed = false;
	};

	bool IsBackgroundWindowUpdateComplete(const Sailor::TSharedPtr<BackgroundWindowUpdateState>& state)
	{
		std::lock_guard lock(state->m_mutex);
		return state->m_completed;
	}

	std::thread StartWindowUpdates(const Sailor::TSharedPtr<BackgroundWindowUpdateState>& state)
	{
		return std::thread([state]()
			{
				@autoreleasepool
				{
					try
					{
						std::string temporaryTitle = WorkerTitle;
						state->m_window->SetWindowTitle(temporaryTitle.c_str());
						temporaryTitle.assign("Caller reused its title buffer");
						state->m_window->ChangeWindowSize(TestWidth, TestHeight, false);
					}
					catch (...)
					{
						state->m_failure = std::current_exception();
					}
				}

				{
					std::lock_guard lock(state->m_mutex);
					state->m_completed = true;
				}
				state->m_completedCondition.notify_one();
			});
	}

	bool FinishWindowUpdates(const Sailor::TSharedPtr<BackgroundWindowUpdateState>& state, std::thread& updateThread)
	{
		bool completedWithoutMainQueuePump = false;
		{
			std::unique_lock lock(state->m_mutex);
			completedWithoutMainQueuePump = state->m_completedCondition.wait_for(
				lock,
				std::chrono::seconds(2),
				[state]() { return state->m_completed; });
		}

		// A dispatch_sync(main) regression leaves the worker blocked. Pump the
		// main run loop before failing so the queued updates can finish and join.
		bool completedEventually = completedWithoutMainQueuePump;
		if (!completedEventually)
		{
			completedEventually = PumpMainRunLoopUntil(
				[state]() { return IsBackgroundWindowUpdateComplete(state); },
				std::chrono::seconds(2));
		}

		if (!completedEventually)
		{
			// The shared state owns the Window, so detaching cannot leave the
			// worker with stack references while this standalone test exits.
			updateThread.detach();
			throw std::runtime_error("background window updates did not complete during cleanup");
		}

		updateThread.join();
		return completedWithoutMainQueuePump;
	}

	void TestWindowUpdatesFromWorkerDoNotWaitForMainQueue()
	{
		Require([NSThread isMainThread], "mac window threading test must start on the main thread");

		auto window = Sailor::TSharedPtr<Window>::Make();
		Require(window->Create("Sailor window threading test", "SailorWindowThreadingTest", 128, 96, false, false, nullptr),
			"test should create a real macOS window");
		window->Show(false);

		NSWindow* nativeWindow = (__bridge NSWindow*)window->GetHWND();
		Require(nativeWindow != nil, "created Sailor window should expose an NSWindow");

		auto state = Sailor::TSharedPtr<BackgroundWindowUpdateState>::Make();
		state->m_window = window;
		auto updateThread = StartWindowUpdates(state);
		const bool completedWithoutMainQueuePump = FinishWindowUpdates(state, updateThread);

		DrainMainQueue();

		const NSSize contentSize = nativeWindow.contentView.bounds.size;
		const bool nativeSizeApplied = std::abs(contentSize.width - TestWidth) < 0.5 &&
			std::abs(contentSize.height - TestHeight) < 0.5;
		const bool trackedSizeApplied = window->GetWidth() == TestWidth && window->GetHeight() == TestHeight;
		const bool titleApplied = [nativeWindow.title isEqualToString:[NSString stringWithUTF8String:WorkerTitle]];
		CAMetalLayer* metalLayer = (__bridge CAMetalLayer*)window->GetMetalLayer();
		const CGFloat backingScale = std::max<CGFloat>(nativeWindow.backingScaleFactor, 1.0);
		const bool drawableSizeApplied = metalLayer &&
			std::abs(metalLayer.drawableSize.width - TestWidth * backingScale) < 0.5 &&
			std::abs(metalLayer.drawableSize.height - TestHeight * backingScale) < 0.5;
		const std::exception_ptr backgroundFailure = state->m_failure;

		// Window::Destroy closes the NSWindow. Suppress the production delegate's
		// process-termination behavior in this standalone test executable.
		nativeWindow.delegate = nil;
		window->Destroy();

		if (backgroundFailure)
		{
			std::rethrow_exception(backgroundFailure);
		}
		Require(completedWithoutMainQueuePump,
			"background window updates must not synchronously wait for the main queue");
		Require(titleApplied, "queued window title must preserve UTF-8 after the caller reuses its buffer");
		Require(trackedSizeApplied, "background ChangeWindowSize should update tracked dimensions");
		Require(nativeSizeApplied, "queued main-thread resize should update the NSWindow content size");
		Require(drawableSizeApplied, "queued resize must update the Metal drawable to the current backing-pixel size");
	}

	void TestWindowTitleOnMainThread()
	{
		Window window;
		window.SetWindowTitle("Before creation");
		Require(window.Create("Sailor title test", "SailorTitleTest", 128, 96, false, false, nullptr),
			"title test should create a real macOS window");
		window.Show(false);
		NSWindow* nativeWindow = (__bridge NSWindow*)window.GetHWND();
		nativeWindow.delegate = nil;
		window.SetWindowTitle(WorkerTitle);
		Require([nativeWindow.title isEqualToString:[NSString stringWithUTF8String:WorkerTitle]],
			"main-thread title update should immediately reach AppKit");
		window.SetWindowTitle("\xFF");
		Require([nativeWindow.title isEqualToString:[NSString stringWithUTF8String:WorkerTitle]],
			"invalid UTF-8 must leave the existing title intact");
		window.SetWindowTitle(nullptr);
		Require([nativeWindow.title isEqualToString:@""], "null title should clear the title");
		window.Destroy();
		window.SetWindowTitle("After destruction");
	}

	void TestQueuedWindowUpdatesSurviveDestruction()
	{
		auto window = Sailor::TSharedPtr<Window>::Make();
		Require(window->Create("Sailor queued update test", "SailorQueuedUpdateTest", 128, 96, false, true, nullptr),
			"queued update test should create a real macOS window");
		window->Show(false);
		NSWindow* nativeWindow = [(__bridge NSWindow*)window->GetHWND() retain];
		nativeWindow.delegate = nil;

		auto state = Sailor::TSharedPtr<BackgroundWindowUpdateState>::Make();
		state->m_window = window;
		auto updateThread = StartWindowUpdates(state);
		const bool completedWithoutMainQueuePump = FinishWindowUpdates(state, updateThread);
		state->m_window.Clear();
		window.Clear();
		DrainMainQueue();

		const bool titleApplied = [nativeWindow.title isEqualToString:[NSString stringWithUTF8String:WorkerTitle]];
		const NSSize contentSize = nativeWindow.contentView.bounds.size;
		const bool sizeApplied = std::abs(contentSize.width - TestWidth) < 0.5 &&
			std::abs(contentSize.height - TestHeight) < 0.5;
		CAMetalLayer* metalLayer = (CAMetalLayer*)nativeWindow.contentView.layer;
		const bool vsyncPreserved = metalLayer.displaySyncEnabled;
		[nativeWindow release];

		if (state->m_failure) std::rethrow_exception(state->m_failure);
		Require(completedWithoutMainQueuePump, "queued updates must finish before the test pumps the main queue");
		Require(titleApplied, "queued title should own its native window until the update completes");
		Require(sizeApplied, "queued resize should survive destruction of the C++ window");
		Require(vsyncPreserved, "queued resize should preserve the requested vsync setting");
	}

	bool TestMouseCaptureLifecycle()
	{
		Window window;
		window.RequestMouseCapture(true);
		window.UpdateMouseCapture();
		Require(!window.IsMouseCaptured(), "capture should require a native window");
		window.AddMouseDelta(10.0f, -5.0f);
		Require(window.ConsumeMouseDelta() == glm::vec2(0.0f), "uncaptured mouse movement should be ignored");
		window.RequestMouseCapture(false);

		Require(window.Create("Sailor mouse capture test", "SailorMouseCaptureTest", 128, 96, false, false, nullptr),
			"mouse capture test should create a real macOS window");
		NSWindow* nativeWindow = (__bridge NSWindow*)window.GetHWND();
		const bool bHasFocus = PumpMainRunLoopUntil([&window]()
		{
			window.ProcessSystemMessages();
			return window.IsActive();
		}, std::chrono::seconds(2));
		if (!bHasFocus)
		{
			return false;
		}

		std::thread requestThread([&window]() { window.RequestMouseCapture(true); });
		requestThread.join();
		Require(!window.IsMouseCaptured(), "worker capture requests should wait for the window thread");
		window.UpdateMouseCapture();
		if (!window.IsMouseCaptured())
		{
			return false;
		}

		// Window's destructor releases capture even if a check below fails.
		CGEventRef mouseEvent = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, CGPointZero, kCGMouseButtonLeft);
		Require(mouseEvent != nullptr, "native relative mouse event should be constructible");
		CGEventSetIntegerValueField(mouseEvent, kCGMouseEventDeltaX, 12);
		CGEventSetIntegerValueField(mouseEvent, kCGMouseEventDeltaY, -7);
		NSEvent* event = [NSEvent eventWithCGEvent:mouseEvent];
		CFRelease(mouseEvent);
		Require(event != nil, "native relative mouse event should reach AppKit");
		[nativeWindow.contentView mouseMoved:event];
		window.AddMouseDelta(-2.0f, 3.0f);
		Require(window.ConsumeMouseDelta() == glm::vec2(10.0f, -4.0f),
			"captured native movement should accumulate relative deltas");
		Require(window.ConsumeMouseDelta() == glm::vec2(0.0f), "consuming movement should clear the accumulated delta");

		window.AddMouseDelta(100.0f, 100.0f);
		window.Show(false);
		Require(!window.IsActive() && !window.IsMouseCaptured(), "losing native focus should immediately release capture");
		Require(window.ConsumeMouseDelta() == glm::vec2(0.0f), "losing focus should discard pending movement");
		window.UpdateMouseCapture();
		Require(!window.IsMouseCaptured(), "an unfocused window must not recapture the mouse");

		window.Show(true);
		Require(window.IsMouseCaptured(), "regaining focus should restore requested capture");
		window.UpdateMouseCapture();
		window.UpdateMouseCapture();
		window.RequestMouseCapture(false);
		window.UpdateMouseCapture();
		Require(!window.IsMouseCaptured(), "canceling a request should release capture");
		window.AddMouseDelta(100.0f, 100.0f);
		Require(window.ConsumeMouseDelta() == glm::vec2(0.0f), "released mouse movement should be ignored");

		window.RequestMouseCapture(true);
		window.UpdateMouseCapture();
		Require(window.IsMouseCaptured(), "capture should work again after an explicit release");
		window.AddMouseDelta(100.0f, 100.0f);
		window.Destroy();
		Require(!window.IsMouseCaptured(), "destroying a captured window should release the cursor");
		Require(window.ConsumeMouseDelta() == glm::vec2(0.0f), "destroying a window should discard pending movement");
		return true;
	}

	void TestNativeFocusLossOnFrameOwner()
	{
		using namespace Sailor::Win32;
		Window window;
		Require(window.Create("Sailor focus input test", "SailorFocusInputTest", 128, 96, false, false, nullptr),
			"focus test should create the native input adapter");
		window.Show(false);
		NSWindow* nativeWindow = (__bridge NSWindow*)window.GetHWND();
		id<NSWindowDelegate> nativeDelegate = [nativeWindow.delegate retain];
		nativeWindow.delegate = nil;
		NSView* view = nativeWindow.contentView;
		GlobalInput::ProcessPendingEvents(false);
		GlobalInput::Reset();
		NSEvent* key = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:0
			windowNumber:nativeWindow.windowNumber context:nil characters:@"w" charactersIgnoringModifiers:@"w" isARepeat:NO keyCode:0x0D];
		NSEvent* mouse = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:NSMakePoint(12, 21)
			modifierFlags:0 timestamp:0 windowNumber:nativeWindow.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1];
		[view keyDown:key];
		[view mouseDown:mouse];
		InputState pressed;
		std::thread pressOwner([&]
		{
			GlobalInput::ProcessPendingEvents(true);
			pressed = GlobalInput::GetInputState();
		});
		pressOwner.join();
		[nativeDelegate windowDidResignKey:[NSNotification notificationWithName:NSWindowDidResignKeyNotification object:nativeWindow]];
		const bool bWasStillPressed = GlobalInput::GetInputState().IsKeyDown('W');
		InputState released;
		std::thread releaseOwner([&]
		{
			GlobalInput::ProcessPendingEvents(true);
			released = GlobalInput::GetInputState();
		});
		releaseOwner.join();
		[nativeDelegate windowDidBecomeKey:[NSNotification notificationWithName:NSWindowDidBecomeKeyNotification object:nativeWindow]];
		GlobalInput::ProcessPendingEvents(true);
		[nativeDelegate release];
		Require(pressed.IsKeyPressed('W') && pressed.IsButtonClick(VK_LBUTTON),
			"real native key/button callbacks must reach the separate frame owner");
		Require(bWasStillPressed && !released.IsKeyDown('W') && !released.IsButtonDown(VK_LBUTTON),
			"native focus loss must release input on the frame owner, not inside the UI callback");
		Require(released.GetCursorPos() == pressed.GetCursorPos() && released.GetButtonPressCursorPos(VK_LBUTTON) == glm::ivec2{},
			"focus loss must retain cursor coordinates while clearing old press origins");
		Require(!GlobalInput::GetInputState().IsKeyDown('W'), "returning focus must not restore a key released outside the window");
		window.Destroy();
	}

	void TestNativeModifierSides()
	{
		using namespace Sailor::Win32;
		Window window;
		Require(window.Create("Sailor modifier input test", "SailorModifierInputTest", 128, 96, false, false, nullptr),
			"modifier test needs the production native input adapter");
		window.Show(false);
		NSWindow* nativeWindow = (__bridge NSWindow*)window.GetHWND();
		nativeWindow.delegate = nil;
		NSView* view = nativeWindow.contentView;
		auto dispatch = [&](unsigned short code, NSEventModifierFlags flags)
		{
			NSEvent* event = [NSEvent keyEventWithType:NSEventTypeFlagsChanged location:NSZeroPoint modifierFlags:flags timestamp:0
				windowNumber:nativeWindow.windowNumber context:nil characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:code];
			[view flagsChanged:event];
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
		struct Modifier
		{
			unsigned short native[2];
			uint32_t key[2];
			uint32_t aggregate;
			NSEventModifierFlags side[2];
			NSEventModifierFlags flags;
		};
		const Modifier modifiers[] = {
			{ { 0x38, 0x3C }, { VK_LSHIFT, VK_RSHIFT }, VK_SHIFT, { NX_DEVICELSHIFTKEYMASK, NX_DEVICERSHIFTKEYMASK }, NSEventModifierFlagShift },
			{ { 0x3B, 0x3E }, { VK_LCONTROL, VK_RCONTROL }, VK_CONTROL, { NX_DEVICELCTLKEYMASK, NX_DEVICERCTLKEYMASK }, NSEventModifierFlagControl },
			{ { 0x3A, 0x3D }, { VK_LMENU, VK_RMENU }, VK_MENU, { NX_DEVICELALTKEYMASK, NX_DEVICERALTKEYMASK }, NSEventModifierFlagOption },
			{ { 0x37, 0x36 }, { VK_LWIN, VK_RWIN }, 0, { NX_DEVICELCMDKEYMASK, NX_DEVICERCMDKEYMASK }, NSEventModifierFlagCommand }
		};
		for (const auto& modifier : modifiers)
		{
			for (uint32_t first : { 0u, 1u })
			{
				const uint32_t second = 1u - first;
				GlobalInput::ProcessPendingEvents(false);
				GlobalInput::Reset();
				dispatch(modifier.native[first], modifier.flags | modifier.side[first]);
				Require(!GlobalInput::GetInputState().IsKeyDown(modifier.key[first]), "flagsChanged must only enqueue input");
				auto state = drain();
				Require(state.IsKeyDown(modifier.key[first]) && !state.IsKeyDown(modifier.key[second]) &&
					(!modifier.aggregate || state.IsKeyDown(modifier.aggregate)), "the native modifier must retain its physical side");
				dispatch(modifier.native[second], modifier.flags | modifier.side[0] | modifier.side[1]);
				dispatch(modifier.native[first], modifier.flags | modifier.side[second]);
				state = drain();
				Require(!state.IsKeyDown(modifier.key[first]) && state.IsKeyDown(modifier.key[second]) &&
					(!modifier.aggregate || state.IsKeyDown(modifier.aggregate)), "releasing one native side must preserve its held sibling");
				dispatch(modifier.native[second], 0);
				state = drain();
				Require(!state.IsKeyDown(modifier.key[0]) && !state.IsKeyDown(modifier.key[1]) &&
					(!modifier.aggregate || !state.IsKeyDown(modifier.aggregate)), "the last native release must clear the modifier");
			}
		}
		window.Destroy();
	}

	void TestNativeKeyboardDispatchPreservesGameplayControls()
	{
		using Sailor::Win32::GlobalInput;
		Window window;
		Require(window.Create("Sailor input test", "SailorInputTest", 128, 96, false, false, nullptr),
			"input test should create a real macOS content view");
		window.Show(false);
		NSWindow* nativeWindow = (__bridge NSWindow*)window.GetHWND();
		// Keep failed assertions from triggering the standalone close-to-quit path.
		nativeWindow.delegate = nil;
		NSView* view = nativeWindow.contentView;
		Require(view != nil, "input test needs the production content view");
		auto dispatchKey = [&](unsigned short code, bool down, bool repeat = false)
		{
			NSEvent* event = [NSEvent keyEventWithType:(down ? NSEventTypeKeyDown : NSEventTypeKeyUp)
				location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:nativeWindow.windowNumber
				context:nil characters:@"" charactersIgnoringModifiers:@"" isARepeat:repeat keyCode:code];
			Require(event != nil, "native key event should be constructible");
			if (down) [view keyDown:event];
			else [view keyUp:event];
		};

		struct Key { unsigned short native; uint32_t engine; };
		constexpr Key keys[] = {
			{ 0x25, 'L' }, { 0x17, '5' }, { 0x16, '6' },
			{ 0x0D, 'W' }, { 0x00, 'A' }, { 0x01, 'S' }, { 0x02, 'D' },
			{ 0x0E, 'E' }, { 0x0F, 'R' }, { 0x03, 'F' }, { 0x05, 'G' },
			{ 0x10, 'Y' }, { 0x11, 'T' }, { 0x12, '1' }, { 0x13, '2' },
			{ 0x14, '3' }, { 0x15, '4' }, { 0x31, 0x20 }, { 0x35, VK_ESCAPE }
		};
		for (const auto& key : keys)
		{
			GlobalInput::ProcessPendingEvents(false);
			GlobalInput::Reset();
			dispatchKey(key.native, true);
			Require(!GlobalInput::GetInputState().IsKeyDown(key.engine),
				"native UI callbacks must not mutate gameplay input before the frame owner consumes them");
			std::thread frameOwner([] { GlobalInput::ProcessPendingEvents(true); });
			frameOwner.join();
			Require(GlobalInput::GetInputState().IsKeyPressed(key.engine),
				"native key-down must publish gameplay key " + std::to_string(key.engine));
			dispatchKey(key.native, false);
			GlobalInput::ProcessPendingEvents(true);
			Require(!GlobalInput::GetInputState().IsKeyDown(key.engine),
				"native key-up must release gameplay key " + std::to_string(key.engine));
			dispatchKey(key.native, true, true);
			GlobalInput::ProcessPendingEvents(true);
			Require(!GlobalInput::GetInputState().IsKeyDown(key.engine),
				"native auto-repeat must not synthesize another gameplay press");
		}
		GlobalInput::Reset();
		dispatchKey(0xFFFF, true);
		GlobalInput::ProcessPendingEvents(true);
		for (uint32_t key = 0; key < 256; ++key)
			Require(!GlobalInput::GetInputState().IsKeyDown(key), "unmapped native keys must not alter gameplay input");
		GlobalInput::Reset();
		window.Destroy();
	}
}

int main()
{
	@autoreleasepool
	{
		try
		{
			TestWindowUpdatesFromWorkerDoNotWaitForMainQueue();
			std::cout << "[PASS] WindowUpdatesFromWorkerDoNotWaitForMainQueue" << std::endl;
			TestWindowTitleOnMainThread();
			std::cout << "[PASS] WindowTitleOnMainThread" << std::endl;
			TestQueuedWindowUpdatesSurviveDestruction();
			std::cout << "[PASS] QueuedWindowUpdatesSurviveDestruction" << std::endl;
			if (TestMouseCaptureLifecycle())
			{
				std::cout << "[PASS] MouseCaptureLifecycle" << std::endl;
			}
			else
			{
				std::cout << "[SKIP] MouseCaptureLifecycle: foreground desktop or cursor capture is unavailable" << std::endl;
			}
			TestNativeKeyboardDispatchPreservesGameplayControls();
			std::cout << "[PASS] NativeKeyboardDispatchPreservesGameplayControls" << std::endl;
			TestNativeFocusLossOnFrameOwner();
			std::cout << "[PASS] NativeFocusLossOnFrameOwner" << std::endl;
			TestNativeModifierSides();
			std::cout << "[PASS] NativeModifierSides" << std::endl;
			return 0;
		}
		catch (const std::exception& exception)
		{
			std::cerr << "[FAIL] MacWindowThreadingTests: " << exception.what() << std::endl;
			return 1;
		}
	}
}

#else
int main()
{
	return 0;
}
#endif
