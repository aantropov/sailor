#include "Input.h"

using namespace Sailor;
using namespace Sailor::Win32;

InputState Input::m_rawState;

Input::Input()
{
	memset(&m_rawState, 1, sizeof(InputState));
}

bool Input::IsKeyDown(uint32_t key)
{
	return (m_rawState.m_keyboard[key] != KeyState::Up);
}

bool Input::IsKeyPressed(uint32_t key)
{
	bool pressed = (m_rawState.m_keyboard[key] == KeyState::Pressed);
	m_rawState.m_keyboard[key] = KeyState::Down;
	return pressed;
}

bool Input::IsButtonDown(uint32_t button)
{
	return (m_rawState.m_keyboard[button] != KeyState::Up);
}

bool Input::IsButtonClick(uint32_t  button)
{
	bool pressed = (m_rawState.m_keyboard[button] == KeyState::Pressed);
	m_rawState.m_keyboard[button] = KeyState::Down;
	return pressed;
}

void Input::GetCursorPos(int32_t* x, int32_t* y)
{
	*x = m_rawState.m_cursorPosition[0];
	*y = m_rawState.m_cursorPosition[1];
}

void Input::SetCursorPos(int32_t x, int32_t y)
{
	POINT Pos = { x, y };
	//ClientToScreen(URenderer::GetInstance()->GetHWND(), &pos);
	::SetCursorPos(Pos.x, Pos.y);

	m_rawState.m_cursorPosition[0] = x;
	m_rawState.m_cursorPosition[1] = y;
}

void Input::ShowCursor(bool bIsVisible)
{
	::ShowCursor(bIsVisible ? TRUE : FALSE);
}