#if defined(__APPLE__)

#include "Platform/Win32/Window.h"
#include "Platform/Win32/Input.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <dispatch/dispatch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

using Sailor::Win32::Window;

namespace
{
	constexpr int32_t TestWidth = 321;
	constexpr int32_t TestHeight = 177;

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
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

	struct BackgroundResizeState
	{
		std::shared_ptr<Window> m_window;
		std::mutex m_mutex;
		std::condition_variable m_completedCondition;
		std::exception_ptr m_failure;
		bool m_completed = false;
	};

	bool IsBackgroundResizeComplete(const std::shared_ptr<BackgroundResizeState>& state)
	{
		std::lock_guard lock(state->m_mutex);
		return state->m_completed;
	}

	void TestWindowResizeFromWorkerDoesNotWaitForMainQueue()
	{
		Require([NSThread isMainThread], "mac window threading test must start on the main thread");

		auto window = std::make_shared<Window>();
		Require(window->Create("Sailor window threading test", "SailorWindowThreadingTest", 128, 96, false, false, nullptr),
			"test should create a real macOS window");
		window->Show(false);

		NSWindow* nativeWindow = (__bridge NSWindow*)window->GetHWND();
		Require(nativeWindow != nil, "created Sailor window should expose an NSWindow");

		auto state = std::make_shared<BackgroundResizeState>();
		state->m_window = window;
		std::thread resizeThread([state]()
			{
				@autoreleasepool
				{
					try
					{
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

		bool completedWithoutMainQueuePump = false;
		{
			std::unique_lock lock(state->m_mutex);
			completedWithoutMainQueuePump = state->m_completedCondition.wait_for(
				lock,
				std::chrono::seconds(2),
				[state]() { return state->m_completed; });
		}

		// A dispatch_sync(main) regression leaves the worker blocked. Pump the
		// main run loop before failing so the queued resize can finish and join.
		bool completedEventually = completedWithoutMainQueuePump;
		if (!completedEventually)
		{
			completedEventually = PumpMainRunLoopUntil(
				[state]() { return IsBackgroundResizeComplete(state); },
				std::chrono::seconds(2));
		}

		if (!completedEventually)
		{
			// The shared state owns the Window, so detaching cannot leave the
			// worker with stack references while this standalone test exits.
			resizeThread.detach();
			throw std::runtime_error("background window resize did not complete during cleanup");
		}

		resizeThread.join();

		// Drain all main-queue work submitted before this fence. This applies an
		// asynchronous resize before inspecting or destroying the native window.
		auto mainQueueFence = std::make_shared<std::atomic_bool>(false);
		dispatch_async(dispatch_get_main_queue(), ^
			{
				mainQueueFence->store(true, std::memory_order_release);
			});
		const bool drainedMainQueue = PumpMainRunLoopUntil(
			[mainQueueFence]() { return mainQueueFence->load(std::memory_order_acquire); },
			std::chrono::seconds(2));

		if (!drainedMainQueue)
		{
			throw std::runtime_error("main queue did not drain during window resize cleanup");
		}

		const NSSize contentSize = nativeWindow.contentView.bounds.size;
		const bool nativeSizeApplied = std::abs(contentSize.width - TestWidth) < 0.5 &&
			std::abs(contentSize.height - TestHeight) < 0.5;
		const bool trackedSizeApplied = window->GetWidth() == TestWidth && window->GetHeight() == TestHeight;
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
			"background ChangeWindowSize must not synchronously wait for the main queue");
		Require(trackedSizeApplied, "background ChangeWindowSize should update tracked dimensions");
		Require(nativeSizeApplied, "queued main-thread resize should update the NSWindow content size");
		Require(drawableSizeApplied, "queued resize must update the Metal drawable to the current backing-pixel size");
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
			GlobalInput::Reset();
			dispatchKey(key.native, true);
			Require(GlobalInput::GetInputState().IsKeyPressed(key.engine),
				"native key-down must publish gameplay key " + std::to_string(key.engine));
			dispatchKey(key.native, false);
			Require(!GlobalInput::GetInputState().IsKeyDown(key.engine),
				"native key-up must release gameplay key " + std::to_string(key.engine));
			dispatchKey(key.native, true, true);
			Require(!GlobalInput::GetInputState().IsKeyDown(key.engine),
				"native auto-repeat must not synthesize another gameplay press");
		}
		GlobalInput::Reset();
		dispatchKey(0xFFFF, true);
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
			TestWindowResizeFromWorkerDoesNotWaitForMainQueue();
			std::cout << "[PASS] WindowResizeFromWorkerDoesNotWaitForMainQueue" << std::endl;
			TestNativeKeyboardDispatchPreservesGameplayControls();
			std::cout << "[PASS] NativeKeyboardDispatchPreservesGameplayControls" << std::endl;
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
