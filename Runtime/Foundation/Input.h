#pragma once
#include <combaseapi.h>

namespace Sailor
{
	LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	
	enum class EKeyState
	{
		Up = 0,
		Down,
		Pressed
	};

	struct FInputState
	{
		EKeyState Keyboard[256];
		EKeyState Mouse[3];
		int CursorPosition[2];
	};

	class GInput
	{
	public:

		GInput();
		
		static bool IsKeyDown(unsigned short int key);
		static bool IsKeyPressed(unsigned short int key);
		static bool IsButtonDown(unsigned short int button);
		static bool IsButtonClick(unsigned short int button);

		static void GetCursorPos(int* x, int* y);
		static void SetCursorPos(int x, int y);
		static void ShowCursor(bool visible);

	protected:

		static FInputState RawState;

		friend LRESULT Sailor::WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	};
}
