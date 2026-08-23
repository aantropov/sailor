#include "AssetRegistry/GlobalIllumination/ProbeVolumeSampling.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Sailor;

namespace
{
	bool Contains(const ProbeVolumeBrick& brick, const glm::vec3& position) noexcept
	{
		return glm::all(glm::greaterThanEqual(position, brick.m_min)) &&
			glm::all(glm::lessThanEqual(position, brick.m_max));
	}

	uint32_t FlattenIndex(
		const glm::uvec3& coordinate,
		const glm::uvec3& counts) noexcept
	{
		return coordinate.x + counts.x *
			(coordinate.y + counts.y * coordinate.z);
	}

	uint32_t VisibilityDirectionIndex(const glm::vec3& direction) noexcept
	{
		const glm::vec3 absolute = glm::abs(direction);
		if (absolute.x >= absolute.y && absolute.x >= absolute.z)
		{
			return direction.x >= 0.0f ? 0u : 1u;
		}
		if (absolute.y >= absolute.z)
		{
			return direction.y >= 0.0f ? 2u : 3u;
		}
		return direction.z >= 0.0f ? 4u : 5u;
	}

	float VisibilityWeight(
		const ProbeVolumeSample& probe,
		const glm::vec3& worldPosition) noexcept
	{
		const glm::vec3 delta = worldPosition - probe.m_position;
		const float distance = glm::length(delta);
		if (!std::isfinite(distance) || distance <= 1e-5f)
		{
			return 1.0f;
		}
		const glm::vec2 moments = probe.m_visibility[
			VisibilityDirectionIndex(delta / distance)];
		if (moments.x <= 0.0f && moments.y <= 0.0f)
		{
			return 1.0f;
		}
		if (distance <= moments.x)
		{
			return 1.0f;
		}
		const float variance = (std::max)(
			moments.y - moments.x * moments.x,
			1e-5f);
		const float difference = distance - moments.x;
		const float chebyshev = variance /
			(variance + difference * difference);
		return chebyshev * chebyshev * chebyshev;
	}
}

glm::vec3 Sailor::EvaluateProbeIrradianceSH(
	const std::array<glm::vec3,
		ProbeVolumeSphericalHarmonicsCoefficientCount>& coefficients,
	const glm::vec3& sourceNormal) noexcept
{
	glm::vec3 normal = sourceNormal;
	const float length = glm::length(normal);
	if (!std::isfinite(length) || length <= 1e-6f)
	{
		normal = glm::vec3(0.0f, 1.0f, 0.0f);
	}
	else
	{
		normal /= length;
	}
	const float x = normal.x;
	const float y = normal.y;
	const float z = normal.z;
	const float basis[ProbeVolumeSphericalHarmonicsCoefficientCount]
	{
		0.2820947918f,
		0.4886025119f * y,
		0.4886025119f * z,
		0.4886025119f * x,
		1.0925484306f * x * y,
		1.0925484306f * y * z,
		0.3153915653f * (3.0f * z * z - 1.0f),
		1.0925484306f * x * z,
		0.5462742153f * (x * x - y * y)
	};
	glm::vec3 irradiance(0.0f);
	for (uint32_t index = 0u;
		index < ProbeVolumeSphericalHarmonicsCoefficientCount;
		++index)
	{
		irradiance += coefficients[index] * basis[index];
	}
	return glm::max(irradiance, glm::vec3(0.0f));
}

bool Sailor::SampleProbeVolumeIrradiance(
	const ProbeVolumeData& data,
	const glm::vec3& worldPosition,
	const glm::vec3& worldNormal,
	glm::vec3& outIrradiance,
	ProbeVolumeSampleDebugInfo* outDebugInfo) noexcept
{
	outIrradiance = glm::vec3(0.0f);
	if (outDebugInfo)
	{
		*outDebugInfo = ProbeVolumeSampleDebugInfo{};
	}
	if (data.m_bricks.IsEmpty() || data.m_probes.IsEmpty())
	{
		return false;
	}

	const ProbeVolumeBrick* selectedBrick = nullptr;
	uint32_t selectedBrickIndex = 0u;
	for (uint32_t index = 0u; index < data.m_bricks.Num(); ++index)
	{
		const ProbeVolumeBrick& candidate = data.m_bricks[index];
		if (Contains(candidate, worldPosition) &&
			(!selectedBrick ||
				candidate.m_subdivisionLevel > selectedBrick->m_subdivisionLevel))
		{
			selectedBrick = &candidate;
			selectedBrickIndex = index;
		}
	}
	if (!selectedBrick)
	{
		return false;
	}

	const glm::vec3 extent = selectedBrick->m_max - selectedBrick->m_min;
	if (glm::any(glm::lessThanEqual(extent, glm::vec3(0.0f))) ||
		glm::any(glm::equal(selectedBrick->m_probeCounts, glm::uvec3(0u))))
	{
		return false;
	}
	const glm::vec3 normalized = glm::clamp(
		(worldPosition - selectedBrick->m_min) / extent,
		glm::vec3(0.0f),
		glm::vec3(1.0f));
	const glm::vec3 cell = normalized * glm::vec3(
		glm::max(selectedBrick->m_probeCounts, glm::uvec3(1u)) -
		glm::uvec3(1u));
	const glm::uvec3 lower = glm::uvec3(glm::floor(cell));
	const glm::uvec3 upper = glm::min(
		lower + glm::uvec3(1u),
		selectedBrick->m_probeCounts - glm::uvec3(1u));
	const glm::vec3 fraction = glm::fract(cell);

	std::array<glm::vec3,
		ProbeVolumeSphericalHarmonicsCoefficientCount> blended{};
	float totalWeight = 0.0f;
	uint32_t cornerIndex = 0u;
	for (uint32_t z = 0u; z < 2u; ++z)
	{
		for (uint32_t y = 0u; y < 2u; ++y)
		{
			for (uint32_t x = 0u; x < 2u; ++x, ++cornerIndex)
			{
				const glm::uvec3 coordinate(
					x ? upper.x : lower.x,
					y ? upper.y : lower.y,
					z ? upper.z : lower.z);
				const uint32_t probeIndex = selectedBrick->m_firstProbeIndex +
					FlattenIndex(coordinate, selectedBrick->m_probeCounts);
				if (probeIndex >= data.m_probes.Num())
				{
					return false;
				}
				const ProbeVolumeSample& probe = data.m_probes[probeIndex];
				const float trilinearWeight =
					(x ? fraction.x : 1.0f - fraction.x) *
					(y ? fraction.y : 1.0f - fraction.y) *
					(z ? fraction.z : 1.0f - fraction.z);
				const float weight = trilinearWeight *
					glm::clamp(probe.m_validity, 0.0f, 1.0f) *
					VisibilityWeight(probe, worldPosition);
				for (uint32_t coefficientIndex = 0u;
					coefficientIndex < ProbeVolumeSphericalHarmonicsCoefficientCount;
					++coefficientIndex)
				{
					blended[coefficientIndex] +=
						probe.m_irradiance[coefficientIndex] * weight;
				}
				totalWeight += weight;
				if (outDebugInfo)
				{
					outDebugInfo->m_probeIndices[cornerIndex] = probeIndex;
					outDebugInfo->m_weights[cornerIndex] = weight;
				}
			}
		}
	}
	if (!std::isfinite(totalWeight) || totalWeight <= 1e-6f)
	{
		return false;
	}
	for (glm::vec3& coefficient : blended)
	{
		coefficient /= totalWeight;
	}
	outIrradiance = EvaluateProbeIrradianceSH(blended, worldNormal);
	if (outDebugInfo)
	{
		outDebugInfo->m_brickIndex = selectedBrickIndex;
		outDebugInfo->m_totalUnnormalizedWeight = totalWeight;
		for (float& weight : outDebugInfo->m_weights)
		{
			weight /= totalWeight;
		}
	}
	return true;
}
