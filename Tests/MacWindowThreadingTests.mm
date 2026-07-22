#if defined(__APPLE__)

#import <Cocoa/Cocoa.h>
#import <dispatch/dispatch.h>

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

#include "Platform/Win32/Window.h"

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
			return 0;
		}
		catch (const std::exception& exception)
		{
			std::cerr << "[FAIL] WindowResizeFromWorkerDoesNotWaitForMainQueue: " << exception.what() << std::endl;
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
