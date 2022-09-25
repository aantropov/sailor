#include "Input.h"

using namespace Sailor;
using namespace Sailor::Win32;

InputState GlobalInput::m_rawState;

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
	return m_keyboard[button] != KeyState::Up;
}

bool InputState::IsButtonClick(uint32_t button) const
{
	bool pressed = (m_keyboard[button] == KeyState::Pressed);
	return pressed;
}

glm::ivec2 InputState::GetCursorPos() const
{
	return glm::ivec2(m_cursorPosition[0], m_cursorPosition[1]);
}

void InputState::TrackForChanges(const InputState& previousState)
{
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
		if (m_mouse[i] == KeyState::Pressed && previousState.m_mouse[i] == KeyState::Pressed)
		{
			m_mouse[i] = KeyState::Down;
		}
	}
}

GlobalInput::GlobalInput()
{
	memset(&m_rawState, 1, sizeof(InputState));
}

void GlobalInput::SetCursorPos(int32_t x, int32_t y)
{
	::SetCursorPos(x, y);
}

void GlobalInput::ShowCursor(bool bIsVisible)
{
	::ShowCursor(bIsVisible ? TRUE : FALSE);
}
