#include "GlobalIllumination/RuntimeGIProbesGrid.h"

#include "GlobalIllumination/GIProbesData.h"
#include "Math/Math.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Sailor::RuntimeGIProbesInternal
{
	bool TryResolveProbeGrid(const ProbeGridBounds& bounds,
		float spacing,
		uint32_t capacity,
		ProbeGrid& outGrid) noexcept
	{
		if (!std::isfinite(spacing) || spacing <= 0.0f || capacity < 8u)
		{
			return false;
		}

		const glm::vec3 snappedMin = (glm::floor(bounds.m_min / spacing - 0.5f) + 0.5f) * spacing;
		if (!Math::AllFinite(snappedMin))
		{
			return false;
		}

		glm::uvec3 counts(2u);
		uint64_t probeCount = 1u;
		for (uint32_t axis = 0u; axis < 3u; ++axis)
		{
			const double extent =
				(std::max)(0.0, static_cast<double>(bounds.m_max[axis]) - static_cast<double>(snappedMin[axis]));
			const double count = std::ceil(extent / static_cast<double>(spacing)) + 1.0;
			if (!std::isfinite(count) || count > static_cast<double>((std::numeric_limits<uint32_t>::max)()))
			{
				return false;
			}
			counts[axis] = (std::max)(2u, static_cast<uint32_t>(count));
			const float axisMax = snappedMin[axis] + static_cast<float>(counts[axis] - 1u) * spacing;
			if (axisMax < bounds.m_max[axis])
			{
				if (counts[axis] == (std::numeric_limits<uint32_t>::max)())
				{
					return false;
				}
				++counts[axis];
			}
			if (probeCount > capacity / counts[axis])
			{
				return false;
			}
			probeCount *= counts[axis];
		}

		const glm::vec3 gridMax = snappedMin + glm::vec3(counts - glm::uvec3(1u)) * spacing;
		if (!Math::AllFinite(gridMax))
		{
			return false;
		}
		outGrid.m_min = snappedMin;
		outGrid.m_max = gridMax;
		outGrid.m_counts = counts;
		outGrid.m_spacing = spacing;
		return true;
	}

	bool TryBuildSceneProbeGrid(const ProbeGridBounds& bounds,
		float minimumSpacing,
		uint32_t capacity,
		ProbeGrid& outGrid) noexcept
	{
		if (TryResolveProbeGrid(bounds, minimumSpacing, capacity, outGrid))
		{
			return true;
		}

		float lowerSpacing = minimumSpacing;
		float upperSpacing = minimumSpacing;
		ProbeGrid upperGrid;
		bool bFoundUpperBound = false;
		for (uint32_t iteration = 0u; iteration < 31u; ++iteration)
		{
			lowerSpacing = upperSpacing;
			upperSpacing *= 2.0f;
			if (!std::isfinite(upperSpacing))
			{
				return false;
			}
			if (TryResolveProbeGrid(bounds, upperSpacing, capacity, upperGrid))
			{
				bFoundUpperBound = true;
				break;
			}
		}
		if (!bFoundUpperBound)
		{
			return false;
		}

		for (uint32_t iteration = 0u; iteration < 24u; ++iteration)
		{
			const float candidateSpacing =
				static_cast<float>(std::sqrt(static_cast<double>(lowerSpacing) * upperSpacing));
			ProbeGrid candidateGrid;
			if (TryResolveProbeGrid(bounds, candidateSpacing, capacity, candidateGrid))
			{
				upperSpacing = candidateSpacing;
				upperGrid = candidateGrid;
			}
			else
			{
				lowerSpacing = candidateSpacing;
			}
		}
		outGrid = upperGrid;
		return true;
	}

	uint32_t ResolvePublicationLimitedCapacity(const RuntimeGIProbesQualitySettings& settings) noexcept
	{
		const uint64_t fixedBytes = sizeof(GIProbesData) + sizeof(GIProbeBrick);
		if (settings.m_maxDirtyUploadBytesPerFrame <= fixedBytes)
		{
			return 0u;
		}
		const uint64_t capacityFromPublication =
			(settings.m_maxDirtyUploadBytesPerFrame - fixedBytes) / sizeof(GIProbe);
		return static_cast<uint32_t>(
			(std::min)(static_cast<uint64_t>(settings.m_maxActiveProbes), capacityFromPublication));
	}
}
