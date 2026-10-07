#include "Input.h"
#include "Core/SpinLock.h"
#include "Containers/Vector.h"
#include "Submodules/ImGuiApi.h"
#include <algorithm>
#include <utility>
#if defined(_WIN32)
#include <windows.h>
#endif

using namespace Sailor;
using namespace Sailor::Win32;

InputState GlobalInput::m_rawState;

namespace
{
	SpinLock g_nativeInputLock;
	TVector<Platform::InputEvent> g_pendingNativeInput;
	TVector<Platform::InputEvent> g_nativeInputBatch;

	bool TryGetMouseButtonIndex(uint32_t button, uint32_t& index)
	{
		if (button == 0 || button == VK_LBUTTON)
		{
			index = 0;
			return true;
		}

		if (button == VK_RBUTTON)
		{
			index = 1;
			return true;
		}

		if (button == VK_MBUTTON)
		{
			index = 2;
			return true;
		}

		return false;
	}
}

bool InputState::IsKeyDown(uint32_t key) const
{
	return m_keyboard[key] != KeyState::Up;
}

bool InputState::IsKeyPressed(uint32_t key) const
{
	bool pressed = (m_keyboard[key] == KeyState::Pressed);
	return pressed;
}

bool InputState::IsButtonDown(uint32_t button) const
{
	uint32_t index = 0;
	return TryGetMouseButtonIndex(button, index) && m_mouse[index] != KeyState::Up;
}

bool InputState::IsButtonClick(uint32_t button) const
{
	uint32_t index = 0;
	bool pressed = TryGetMouseButtonIndex(button, index) && m_mouse[index] == KeyState::Pressed;
	return pressed;
}

glm::ivec2 InputState::GetCursorPos() const
{
	return glm::ivec2(m_cursorPosition[0], m_cursorPosition[1]);
}

glm::ivec2 InputState::GetButtonPressCursorPos(uint32_t button) const
{
	uint32_t index = 0;
	if (!TryGetMouseButtonIndex(button, index))
	{
		return {};
	}

	return glm::ivec2(m_mousePressPosition[index][0], m_mousePressPosition[index][1]);
}

float InputState::GetMouseWheelDelta() const
{
	return m_mouseWheelDelta;
}

void InputState::TrackForChanges(const InputState& previousState)
{
	m_mouseWheelDelta = m_mouseWheelPosition - previousState.m_mouseWheelPosition;
	for (uint32_t i = 0; i < 256; i++)
	{
		auto prevState = previousState.m_keyboard[i];
		if (m_keyboard[i] == KeyState::Pressed && (prevState == KeyState::Pressed || prevState == KeyState::Down))
		{
			m_keyboard[i] = KeyState::Down;
		}
	}

	for (uint32_t i = 0; i < 3; i++)
	{
		if (m_mouse[i] == KeyState::Pressed && (previousState.m_mouse[i] == KeyState::Pressed || previousState.m_mouse[i] == KeyState::Down))
		{
			m_mouse[i] = KeyState::Down;
		}
	}
}

GlobalInput::GlobalInput()
{
	memset(&m_rawState, 0, sizeof(InputState));
}

const InputState& GlobalInput::GetInputState()
{
	return m_rawState;
}

void GlobalInput::SetCursorPos(int32_t x, int32_t y)
{
#if defined(_WIN32)
	::SetCursorPos(x, y);
#else
	(void)x;
	(void)y;
#endif
}

void GlobalInput::ShowCursor(bool bIsVisible)
{
#if defined(_WIN32)
	::ShowCursor(bIsVisible ? TRUE : FALSE);
#else
	(void)bIsVisible;
#endif
}

void GlobalInput::SetKeyState(uint32_t key, KeyState state)
{
	if (key < 256)
	{
		m_rawState.m_keyboard[key] = state;
		switch (key)
		{
		case VK_LSHIFT:
		case VK_RSHIFT:
			m_rawState.m_keyboard[VK_SHIFT] = std::max(m_rawState.m_keyboard[VK_LSHIFT], m_rawState.m_keyboard[VK_RSHIFT]);
			break;
		case VK_LCONTROL:
		case VK_RCONTROL:
			m_rawState.m_keyboard[VK_CONTROL] = std::max(m_rawState.m_keyboard[VK_LCONTROL], m_rawState.m_keyboard[VK_RCONTROL]);
			break;
		case VK_LMENU:
		case VK_RMENU:
			m_rawState.m_keyboard[VK_MENU] = std::max(m_rawState.m_keyboard[VK_LMENU], m_rawState.m_keyboard[VK_RMENU]);
			break;
		}
	}
}

void GlobalInput::SetMouseButtonState(uint32_t button, KeyState state)
{
	if (button < 3)
	{
		if (state == KeyState::Pressed && m_rawState.m_mouse[button] == KeyState::Up)
		{
			m_rawState.m_mousePressPosition[button][0] = m_rawState.m_cursorPosition[0];
			m_rawState.m_mousePressPosition[button][1] = m_rawState.m_cursorPosition[1];
		}

		m_rawState.m_mouse[button] = state;
		const uint32_t key = button == 0 ? VK_LBUTTON : button == 1 ? VK_RBUTTON : VK_MBUTTON;
		m_rawState.m_keyboard[key] = state;
	}
}

void GlobalInput::SetCursorPosition(int32_t x, int32_t y)
{
	m_rawState.m_cursorPosition[0] = x;
	m_rawState.m_cursorPosition[1] = y;
}

void GlobalInput::AddMouseWheelDelta(float delta)
{
	m_rawState.m_mouseWheelPosition += delta;
}

void GlobalInput::Reset()
{
	m_rawState = {};
}

void GlobalInput::QueueNativeEvent(Platform::InputEvent event)
{
	g_nativeInputLock.Lock();
	g_pendingNativeInput.Add(std::move(event));
	g_nativeInputLock.Unlock();
}

void GlobalInput::ProcessPendingEvents(bool bAcceptNativeInput)
{
	g_nativeInputLock.Lock();
	std::swap(g_nativeInputBatch, g_pendingNativeInput);
	g_nativeInputLock.Unlock();
	if (bAcceptNativeInput)
	{
		for (const auto& event : g_nativeInputBatch) ApplyEvent(event);
	}
	g_nativeInputBatch.Clear(false);
}

void GlobalInput::ApplyEvent(const Platform::InputEvent& event)
{
	using Type = Platform::InputEvent::Type;
	switch (event.m_type)
	{
	case Type::MousePos:
		SetCursorPosition(static_cast<int32_t>(event.m_x), static_cast<int32_t>(event.m_y));
		break;
	case Type::MouseButton:
		SetCursorPosition(static_cast<int32_t>(event.m_x), static_cast<int32_t>(event.m_y));
		if (event.m_button >= 0)
			SetMouseButtonState(static_cast<uint32_t>(event.m_button), event.m_bIsPressed ? KeyState::Pressed : KeyState::Up);
		break;
	case Type::MouseWheel:
		AddMouseWheelDelta(event.m_y);
		break;
	case Type::Key:
		SetKeyState(event.m_key, event.m_bIsPressed ? KeyState::Pressed : KeyState::Up);
		break;
	case Type::Focus:
		if (event.m_bIsPressed) break;
		[[fallthrough]];
	case Type::Reset:
		std::fill(std::begin(m_rawState.m_keyboard), std::end(m_rawState.m_keyboard), KeyState::Up);
		std::fill(std::begin(m_rawState.m_mouse), std::end(m_rawState.m_mouse), KeyState::Up);
		for (auto& position : m_rawState.m_mousePressPosition) position[0] = position[1] = 0;
		// Keep absolute cursor/wheel coordinates: resetting their origins would
		// turn focus loss into a movement/scroll delta in the next FrameState.
		m_rawState.m_mouseWheelDelta = 0.0f;
		break;
	default:
		break;
	}
	ImGuiApi::HandleInput(event);
}
