#pragma once
#include "Sailor.h"

namespace Sailor::Win32
{
#if defined(_WIN32)
	LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

	enum class KeyState : uint8_t
	{
		Up = 0,
		Down,
		Pressed
	};

	struct InputState
	{
		SAILOR_API bool IsKeyDown(uint32_t key) const;
		SAILOR_API bool IsKeyPressed(uint32_t key) const;
		SAILOR_API bool IsButtonDown(uint32_t button) const;
		SAILOR_API bool IsButtonClick(uint32_t button) const;

		SAILOR_API glm::ivec2 GetCursorPos() const;
		SAILOR_API void TrackForChanges(const InputState& previousState);

	protected:

		KeyState m_keyboard[256]{};
		KeyState m_mouse[3]{};
		int32_t m_cursorPosition[2]{};

#if defined(_WIN32)
		friend LRESULT Sailor::Win32::WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif
		friend class GlobalInput;
	};

	class GlobalInput
	{
	public:
		SAILOR_API GlobalInput();

		static SAILOR_API void SetCursorPos(int32_t x, int32_t y);
		static SAILOR_API void ShowCursor(bool bIsVisible);
		static SAILOR_API const InputState& GetInputState() { return m_rawState; }

		SAILOR_API static void SetKeyState(uint32_t key, KeyState state);
		SAILOR_API static void SetMouseButtonState(uint32_t button, KeyState state);
		SAILOR_API static void SetCursorPosition(int32_t x, int32_t y);

	protected:

		static InputState m_rawState;
	};
}
