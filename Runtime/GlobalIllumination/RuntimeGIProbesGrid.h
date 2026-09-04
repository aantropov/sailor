#pragma once

#include "GlobalIllumination/RuntimeGIProbesSettings.h"

#include <glm/vec3.hpp>

#include <cstdint>

namespace Sailor::RuntimeGIProbesInternal
{
	struct ProbeGridBounds final
	{
		glm::vec3 m_min{};
		glm::vec3 m_max{};
	};

	struct ProbeGrid final
	{
		glm::vec3 m_min{};
		glm::vec3 m_max{};
		glm::uvec3 m_counts{2u};
		float m_spacing = 1.0f;
	};

	bool TryBuildSceneProbeGrid(const ProbeGridBounds& bounds,
		float minimumSpacing,
		uint32_t capacity,
		ProbeGrid& outGrid) noexcept;

	uint32_t ResolvePublicationLimitedCapacity(const RuntimeGIProbesQualitySettings& settings) noexcept;
}
