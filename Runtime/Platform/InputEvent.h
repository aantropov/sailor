#pragma once

#include <cstdint>
#include <string>

namespace Sailor::Platform
{
	// Pointer coordinates are client pixels; wheel values are scroll steps.
	// Text owns its bytes because native delivery crosses the UI/Main boundary.
	struct InputEvent
	{
		enum class Type : uint8_t
		{
			MousePos,
			MouseButton,
			MouseWheel,
			Key,
			Text,
			CharacterUtf16,
			Focus,
			Reset
		};

		Type m_type = Type::MousePos;
		float m_x = 0.0f;
		float m_y = 0.0f;
		uint32_t m_key = 0;
		int32_t m_button = -1;
		bool m_bIsPressed = false;
		std::string m_text;
		bool m_bIsKeypad = false;
	};
}
