#pragma once

#include "GlobalIllumination/GIProbesData.h"

#include <array>
#include <cstdint>
#include <limits>

namespace Sailor
{
	struct SAILOR_SHARED_API GIProbeDebugInfo final
	{
		uint32_t m_brickIndex = (std::numeric_limits<uint32_t>::max)();
		std::array<uint32_t, 8u> m_probeIndices{};
		std::array<float, 8u> m_weights{};
		float m_totalUnnormalizedWeight = 0.0f;
	};

	SAILOR_SHARED_API glm::vec3 EvaluateProbeIrradianceSH(
		const std::array<glm::vec3,
			GIProbeSphericalHarmonicsCoefficientCount>& coefficients,
		const glm::vec3& normal) noexcept;

	SAILOR_SHARED_API bool SampleGIProbesIrradiance(
		const GIProbesData& data,
		const glm::vec3& worldPosition,
		const glm::vec3& worldNormal,
		glm::vec3& outIrradiance,
		GIProbeDebugInfo* outDebugInfo = nullptr) noexcept;
}
