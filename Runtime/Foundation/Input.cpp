#include "Input.h"

#include <combaseapi.h>

using namespace Sailor;

InputState Input::m_rawState;

Input::Input()
{
	memset(&m_rawState, 1, sizeof(InputState));
}

bool Input::IsKeyDown(unsigned short int  key)
{
	return (m_rawState.m_keyboard[key] != KeyState::Up);
}

bool Input::IsKeyPressed(unsigned short int  key)
{
	bool pressed = (m_rawState.m_keyboard[key] == KeyState::Pressed);
	m_rawState.m_keyboard[key] = KeyState::Down;
	return pressed;
}

bool Input::IsButtonDown(unsigned short int  button)
{
	return (m_rawState.m_keyboard[button] != KeyState::Up);
}

bool Input::IsButtonClick(unsigned short int  button)
{
	bool pressed = (m_rawState.m_keyboard[button] == KeyState::Pressed);
	m_rawState.m_keyboard[button] = KeyState::Down;
	return pressed;
}

void Input::GetCursorPos(int* x, int* y)
{
	*x = m_rawState.m_cursorPosition[0];
	*y = m_rawState.m_cursorPosition[1];
}

void Input::SetCursorPos(int x, int y)
{
	POINT Pos = { x, y };
	//ClientToScreen(URenderer::GetInstance()->GetHWND(), &pos);
	::SetCursorPos(Pos.x, Pos.y);

	m_rawState.m_cursorPosition[0] = x;
	m_rawState.m_cursorPosition[1] = y;
}

void Input::ShowCursor(bool visible)
{
	::ShowCursor(visible ? TRUE : FALSE);
}