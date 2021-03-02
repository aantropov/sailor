#include "Input.h"

#include <combaseapi.h>

using namespace Sailor;

FInputState GInput::RawState;

GInput::GInput()
{
	memset(&RawState, 1, sizeof(FInputState));
}

bool GInput::IsKeyDown(unsigned short int  key)
{
	return (RawState.Keyboard[key] != EKeyState::Up);
}

bool GInput::IsKeyPressed(unsigned short int  key)
{
	bool pressed = (RawState.Keyboard[key] == EKeyState::Pressed);
	RawState.Keyboard[key] = EKeyState::Down;
	return pressed;
}

bool GInput::IsButtonDown(unsigned short int  button)
{
	return (RawState.Keyboard[button] != EKeyState::Up);
}

bool GInput::IsButtonClick(unsigned short int  button)
{
	bool pressed = (RawState.Keyboard[button] == EKeyState::Pressed);
	RawState.Keyboard[button] = EKeyState::Down;
	return pressed;
}

void GInput::GetCursorPos(int* x, int* y)
{
	*x = RawState.CursorPosition[0];
	*y = RawState.CursorPosition[1];
}

void GInput::SetCursorPos(int x, int y)
{
	POINT Pos = { x, y };
	//ClientToScreen(URenderer::GetInstance()->GetHWND(), &pos);
	::SetCursorPos(Pos.x, Pos.y);

	RawState.CursorPosition[0] = x;
	RawState.CursorPosition[1] = y;
}

void GInput::ShowCursor(bool visible)
{
	::ShowCursor(visible ? TRUE : FALSE);
}