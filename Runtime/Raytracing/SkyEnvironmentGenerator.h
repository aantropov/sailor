#pragma once

#include "Memory/MallocAllocator.hpp"
#include "Memory/LockFreeHeapAllocator.h"
#include "FrameGraph/SkyParameters.h"
#include "Containers/Vector.h"

#include <cstdint>
#include <functional>

namespace Sailor::Raytracing
{
	constexpr uint32_t ProbeBakeSkyEnvironmentWidth = 128u;
	constexpr uint32_t ProbeBakeSkyEnvironmentHeight = 64u;

	using SkyEnvironmentProgressCallback =
		std::function<bool(uint32_t completedRows, uint32_t totalRows)>;

	// Converts the solar source illuminance to the direct normal illuminance
	// reaching the local tangent surface after clear-atmosphere extinction.
	SAILOR_SHARED_API glm::vec3 CalculateDirectSunIlluminance(
		const SkyParameters& parameters);

	// Unobstructed, horizontal Lambertian terrain under this clear sky and sun.
	// This is a distant environment boundary, not a local scene reflection capture.
	SAILOR_SHARED_API glm::vec3 CalculateSkyGroundRadiance(
		const SkyParameters& parameters, const glm::vec3& albedo);

	// Generates the same physically scaled clear-atmosphere lighting used by the
	// runtime sky environment. Clouds, stars, and the explicit sun disk are
	// intentionally excluded: clouds are dynamic, while direct sun comes from
	// LightComponent. Configured ground radiance is the lower-hemisphere boundary.
	SAILOR_SHARED_API bool GenerateSkyEnvironmentEquirectangular(
		const SkyParameters& parameters,
		const glm::uvec2& extent,
		TVector<glm::vec4>& outEnvironment,
		const SkyEnvironmentProgressCallback& progress = {});
}
