#include "Platform/Win32/Input.h"

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

using namespace Sailor;
using namespace Sailor::Win32;
using Sailor::Platform::InputEvent;
using Type = InputEvent::Type;

namespace
{
	void Require(bool condition, std::string_view message)
	{
		if (!condition) throw std::runtime_error(std::string(message));
	}

	void TestOwnedDelivery()
	{
		GlobalInput::ProcessPendingEvents(false);
		GlobalInput::Reset();
		std::jthread native([]
		{
			GlobalInput::QueueNativeEvent({ Type::MouseButton, -23.0f, 71.0f, 0, 0, true });
			GlobalInput::QueueNativeEvent({ Type::Key, 0.0f, 0.0f, 'W', -1, true });
			GlobalInput::QueueNativeEvent({ Type::MouseWheel, 0.5f, 2.5f });
		});
		native.join();
		const auto previous = GlobalInput::GetInputState();
		Require(!previous.IsKeyDown('W') && !previous.IsButtonDown(VK_LBUTTON) && previous.GetCursorPos() == glm::ivec2{},
			"native producers must not publish mutable input before the frame-owner drain");
		GlobalInput::ProcessPendingEvents(true);
		auto current = GlobalInput::GetInputState();
		current.TrackForChanges(previous);
		Require(current.IsKeyPressed('W') && current.IsButtonClick(VK_LBUTTON), "the owner must receive key and button presses");
		Require(current.GetCursorPos() == glm::ivec2(-23, 71) && current.GetButtonPressCursorPos(VK_LBUTTON) == glm::ivec2(-23, 71),
			"a click without a preceding move must carry its signed client coordinates");
		Require(current.GetMouseWheelDelta() == 2.5f, "normalized wheel input must reach the gameplay frame");
		GlobalInput::ProcessPendingEvents(true);
		auto next = GlobalInput::GetInputState();
		next.TrackForChanges(current);
		Require(next.IsKeyDown('W') && !next.IsKeyPressed('W') && next.GetMouseWheelDelta() == 0.0f,
			"an empty drain must not replay events or synthesize a new press");

		GlobalInput::QueueNativeEvent({ Type::Focus });
		GlobalInput::ProcessPendingEvents(true);
		auto released = GlobalInput::GetInputState();
		released.TrackForChanges(next);
		Require(!released.IsKeyDown('W') && !released.IsButtonDown(VK_LBUTTON) &&
			released.GetButtonPressCursorPos(VK_LBUTTON) == glm::ivec2{}, "focus loss must release keys, buttons and press origins");
		Require(released.GetCursorPos() == next.GetCursorPos() && released.GetMouseWheelDelta() == 0.0f,
			"focus loss must not turn reset coordinate origins into artificial movement or scrolling");
		GlobalInput::QueueNativeEvent({ Type::Focus, 0.0f, 0.0f, 0, -1, true });
		GlobalInput::ProcessPendingEvents(true);
		Require(!GlobalInput::GetInputState().IsKeyDown('W'), "focus gain must not restore a key released outside the window");
	}

	void TestPhysicalModifiers()
	{
		struct Keys { uint32_t aggregate; uint32_t left; uint32_t right; };
		for (const auto keys : { Keys{ VK_SHIFT, VK_LSHIFT, VK_RSHIFT },
			Keys{ VK_CONTROL, VK_LCONTROL, VK_RCONTROL }, Keys{ VK_MENU, VK_LMENU, VK_RMENU } })
		{
			GlobalInput::Reset();
			GlobalInput::ApplyEvent({ Type::Key, 0.0f, 0.0f, keys.left, -1, true });
			Require(GlobalInput::GetInputState().IsKeyDown(keys.left) && GlobalInput::GetInputState().IsKeyDown(keys.aggregate),
				"a physical modifier press must retain its identity and set the aggregate modifier");
			const auto first = GlobalInput::GetInputState();
			GlobalInput::ApplyEvent({ Type::Key, 0.0f, 0.0f, keys.right, -1, true });
			GlobalInput::ApplyEvent({ Type::Key, 0.0f, 0.0f, keys.left, -1, false });
			Require(!GlobalInput::GetInputState().IsKeyDown(keys.left) && GlobalInput::GetInputState().IsKeyDown(keys.right) &&
				GlobalInput::GetInputState().IsKeyDown(keys.aggregate), "releasing one side must not release its held sibling");
			auto held = GlobalInput::GetInputState();
			held.TrackForChanges(first);
			Require(!held.IsKeyPressed(keys.aggregate) && held.IsKeyPressed(keys.right),
				"a second physical press must not restart the held aggregate in the next frame");
			GlobalInput::ApplyEvent({ Type::Key, 0.0f, 0.0f, keys.right, -1, false });
			Require(!GlobalInput::GetInputState().IsKeyDown(keys.aggregate), "the aggregate must release with the last physical key");
			GlobalInput::ApplyEvent({ Type::Key, 0.0f, 0.0f, keys.aggregate, -1, true });
			Require(GlobalInput::GetInputState().IsKeyDown(keys.aggregate), "generic remote modifiers must remain supported");
			GlobalInput::ApplyEvent({ Type::Focus });
			GlobalInput::ApplyEvent({ Type::Key, 0.0f, 0.0f, keys.left, -1, true });
			GlobalInput::ApplyEvent({ Type::Key, 0.0f, 0.0f, keys.right, -1, true });
			GlobalInput::ApplyEvent({ Type::Focus });
			Require(!GlobalInput::GetInputState().IsKeyDown(keys.aggregate) && !GlobalInput::GetInputState().IsKeyDown(keys.left) &&
				!GlobalInput::GetInputState().IsKeyDown(keys.right),
				"focus loss must clear both physical and aggregate modifiers");
		}
	}

	void TestHiddenNativeInputIsDiscarded()
	{
		GlobalInput::Reset();
		GlobalInput::ApplyEvent({ Type::Key, 0.0f, 0.0f, 'A', -1, true });
		GlobalInput::QueueNativeEvent({ Type::Focus });
		GlobalInput::QueueNativeEvent({ Type::Key, 0.0f, 0.0f, 'W', -1, true });
		GlobalInput::ProcessPendingEvents(false);
		GlobalInput::ProcessPendingEvents(true);
		Require(GlobalInput::GetInputState().IsKeyDown('A') && !GlobalInput::GetInputState().IsKeyDown('W'),
			"discarded hidden-window callbacks must neither reset remote input nor survive into the next session");
	}

	void TestConcurrentProducers()
	{
		GlobalInput::Reset();
		const auto previous = GlobalInput::GetInputState();
		std::atomic<uint32_t> completed{ 0 };
		std::jthread producers[4];
		for (uint32_t thread = 0; thread < 4; ++thread)
		{
			producers[thread] = std::jthread([&, thread]
			{
				for (uint32_t i = 0; i < 2000; ++i)
				{
					GlobalInput::QueueNativeEvent({ Type::Key, 0.0f, 0.0f, 'A' + thread, -1, (i % 2) == 0 });
					GlobalInput::QueueNativeEvent({ Type::MouseWheel, 0.0f, 1.0f });
				}
				completed.fetch_add(1, std::memory_order_release);
			});
		}
		while (completed.load(std::memory_order_acquire) != 4)
		{
			GlobalInput::ProcessPendingEvents(true);
			std::this_thread::yield();
		}
		for (auto& producer : producers) producer.join();
		GlobalInput::ProcessPendingEvents(true);
		auto current = GlobalInput::GetInputState();
		current.TrackForChanges(previous);
		Require(current.GetMouseWheelDelta() == 8000.0f, "concurrent producers must deliver every wheel event exactly once");
		for (uint32_t key = 'A'; key <= 'D'; ++key)
			Require(!current.IsKeyDown(key), "each producer's final release must follow its press");
	}
}

int main()
{
	try
	{
		TestOwnedDelivery();
		TestPhysicalModifiers();
		TestHiddenNativeInputIsDiscarded();
		TestConcurrentProducers();
		std::cout << "Input owner: queued delivery, focus, coordinates, wheel, source selection and concurrent producers passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
