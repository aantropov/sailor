#pragma once
#include "Sailor.h"

namespace Sailor::Win32
{
	LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

	enum class KeyState : uint8_t
	{
		Up = 0,
		Down,
		Pressed
	};

	struct InputState
	{
		bool SAILOR_API IsKeyDown(uint32_t key) const;
		bool SAILOR_API IsKeyPressed(uint32_t key) const;
		bool SAILOR_API IsButtonDown(uint32_t button) const;
		bool SAILOR_API IsButtonClick(uint32_t button) const;

		glm::ivec2 SAILOR_API GetCursorPos() const;

		void SAILOR_API TrackForChanges(const InputState& previousState);

	protected:

		KeyState m_keyboard[256];
		KeyState m_mouse[3];
		int32_t m_cursorPosition[2];

		friend LRESULT Sailor::Win32::WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
		friend class GlobalInput;
	};

	class GlobalInput
	{
	public:

		SAILOR_API GlobalInput();

		static SAILOR_API void SetCursorPos(int32_t x, int32_t y);
		static SAILOR_API void ShowCursor(bool bIsVisible);

		static SAILOR_API const InputState& GetInputState() { return m_rawState; }

	protected:

		static InputState m_rawState;

		friend LRESULT Sailor::Win32::WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	};
}
