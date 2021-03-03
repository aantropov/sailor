#include "Input.h"

#include <combaseapi.h>

using namespace Sailor;

InputState Input::rawState;

Input::Input()
{
	memset(&rawState, 1, sizeof(InputState));
}

bool Input::IsKeyDown(unsigned short int  key)
{
	return (rawState.keyboard[key] != KeyState::Up);
}

bool Input::IsKeyPressed(unsigned short int  key)
{
	bool pressed = (rawState.keyboard[key] == KeyState::Pressed);
	rawState.keyboard[key] = KeyState::Down;
	return pressed;
}

bool Input::IsButtonDown(unsigned short int  button)
{
	return (rawState.keyboard[button] != KeyState::Up);
}

bool Input::IsButtonClick(unsigned short int  button)
{
	bool pressed = (rawState.keyboard[button] == KeyState::Pressed);
	rawState.keyboard[button] = KeyState::Down;
	return pressed;
}

void Input::GetCursorPos(int* x, int* y)
{
	*x = rawState.cursorPosition[0];
	*y = rawState.cursorPosition[1];
}

void Input::SetCursorPos(int x, int y)
{
	POINT Pos = { x, y };
	//ClientToScreen(URenderer::GetInstance()->GetHWND(), &pos);
	::SetCursorPos(Pos.x, Pos.y);

	rawState.cursorPosition[0] = x;
	rawState.cursorPosition[1] = y;
}

void Input::ShowCursor(bool visible)
{
	::ShowCursor(visible ? TRUE : FALSE);
}