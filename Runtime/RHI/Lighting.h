#pragma once

#include "Math/Math.h"

#include <cstddef>
#include <cstdint>

namespace Sailor::RHI
{
	struct RHILightShaderData
	{
		static constexpr uint32_t InvalidType = static_cast<uint32_t>(-1);

		uint32_t m_type = InvalidType;
		uint32_t m_shadowType = 0u;
		uint32_t m_activeCascadeCount = 1u;
		float m_shadowBias = 0.0f;
		alignas(16) glm::vec3 m_worldPosition{};
		alignas(16) glm::vec3 m_direction{};
		alignas(16) glm::vec3 m_intensity{};
		alignas(16) glm::vec2 m_cutOff{};
		alignas(16) glm::vec3 m_bounds{};
	};

	static_assert(offsetof(RHILightShaderData, m_cutOff) == 64u);
	static_assert(offsetof(RHILightShaderData, m_bounds) == 80u);
	static_assert(sizeof(RHILightShaderData) == 96u);
}
