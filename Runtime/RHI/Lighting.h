#pragma once

#include "Math/Math.h"

#include <cstdint>

namespace Sailor::RHI
{
	struct RHILightShaderData
	{
		static constexpr uint32_t InvalidType = static_cast<uint32_t>(-1);

		uint32_t m_type = InvalidType;
		uint32_t m_shadowType = 0u;
		alignas(16) glm::vec3 m_worldPosition{};
		alignas(16) glm::vec3 m_direction{};
		alignas(16) glm::vec3 m_intensity{};
		alignas(16) glm::vec3 m_attenuation{};
		alignas(16) glm::vec2 m_cutOff{};
		alignas(16) glm::vec3 m_bounds{};
	};
}
