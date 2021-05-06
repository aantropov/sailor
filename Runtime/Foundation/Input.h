#pragma once
#include <combaseapi.h>
#include "Sailor.h"

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

		SAILOR_API Input();
		
		static SAILOR_API bool IsKeyDown(unsigned short int key);
		static SAILOR_API bool IsKeyPressed(unsigned short int key);
		static SAILOR_API bool IsButtonDown(unsigned short int button);
		static SAILOR_API bool IsButtonClick(unsigned short int button);

		static SAILOR_API void GetCursorPos(int* x, int* y);
		static SAILOR_API void SetCursorPos(int x, int y);
		static SAILOR_API void ShowCursor(bool visible);

	protected:

		static InputState m_rawState;

		friend LRESULT Sailor::WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	};
}
