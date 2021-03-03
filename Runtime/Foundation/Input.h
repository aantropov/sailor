#pragma once
#include <combaseapi.h>

namespace Sailor
{
	LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	
	enum class KeyState
	{
		Up = 0,
		Down,
		Pressed
	};

	struct InputState
	{
		KeyState keyboard[256];
		KeyState mouse[3];
		int cursorPosition[2];
	};

	class Input
	{
	public:

		Input();
		
		static bool IsKeyDown(unsigned short int key);
		static bool IsKeyPressed(unsigned short int key);
		static bool IsButtonDown(unsigned short int button);
		static bool IsButtonClick(unsigned short int button);

		static void GetCursorPos(int* x, int* y);
		static void SetCursorPos(int x, int y);
		static void ShowCursor(bool visible);

	protected:

		static InputState rawState;

		friend LRESULT Sailor::WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	};
}
