#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/vec3.hpp>

namespace Sailor
{
	using AudioVoiceId = uint64_t;
	using AudioListenerId = uint64_t;

	constexpr AudioVoiceId InvalidAudioVoiceId = 0;
	constexpr AudioListenerId InvalidAudioListenerId = 0;

	struct AudioVoiceSettings
	{
		float m_volume = 1.0f;
		float m_pitch = 1.0f;
		float m_minDistance = 1.0f;
		float m_maxDistance = 1000.0f;
		bool m_bLoop = false;
		bool m_bSpatial = true;

		bool operator==(const AudioVoiceSettings&) const = default;
	};

	struct AudioTransformState
	{
		glm::vec3 m_position{};
		glm::vec3 m_forward{ 0.0f, 0.0f, -1.0f };
		glm::vec3 m_up{ 0.0f, 1.0f, 0.0f };
		glm::vec3 m_velocity{};
	};

	struct AudioListenerState
	{
		AudioTransformState m_transform{};
		int32_t m_priority = 0;
		bool m_bEnabled = true;
	};
}
