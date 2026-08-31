#include "GlobalIllumination/GIProbesSampling.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Sailor;

namespace
{
	constexpr float SurfacePlaneTolerance = 0.0001f;
	constexpr float MinimumReceiverCoverage = 0.5f;

	bool Contains(const GIProbeBrick& brick, const glm::vec3& position) noexcept
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

	float SurfaceFacingWeight(
		const GIProbe& probe,
		const glm::vec3& worldPosition,
		const glm::vec3& worldNormal,
		float transitionWidth) noexcept
	{
		const float normalLength = glm::length(worldNormal);
		if (!std::isfinite(normalLength) || normalLength <= 1e-6f)
		{
			return 1.0f;
		}
		const glm::vec3 surfaceToProbe = probe.m_position - worldPosition;
		if (!std::isfinite(surfaceToProbe.x) ||
			!std::isfinite(surfaceToProbe.y) ||
			!std::isfinite(surfaceToProbe.z))
		{
			return 0.0f;
		}
		const float signedPlaneDistance = glm::dot(
			worldNormal / normalLength,
			surfaceToProbe);
		const float width = (std::max)(
			transitionWidth,
			SurfacePlaneTolerance);
		const float normalized = glm::clamp(
			(signedPlaneDistance + width) / (2.0f * width),
			0.0f,
			1.0f);
		return normalized * normalized * (3.0f - 2.0f * normalized);
	}

	float ProbeMomentVisibility(
		const GIProbe& probe,
		const glm::vec3& worldPosition,
		const GIProbesBakeSettings& settings) noexcept
	{
		if ((probe.m_flags & GIProbeBlockedDirectionMask) == 0u)
		{
			return 1.0f;
		}
		const glm::vec3 probeToReceiver = worldPosition - probe.m_position;
		const float receiverDistance = glm::length(probeToReceiver);
		if (!std::isfinite(receiverDistance))
		{
			return 0.0f;
		}
		if (receiverDistance <= 1e-6f)
		{
			return 1.0f;
		}

		const glm::vec3 direction = probeToReceiver / receiverDistance;
		const glm::vec3 axisWeights = glm::abs(direction);
		const float totalAxisWeight =
			axisWeights.x + axisWeights.y + axisWeights.z;
		float meanDistance = 0.0f;
		float meanDistanceSquared = 0.0f;
		for (uint32_t axis = 0u; axis < 3u; ++axis)
		{
			const uint32_t directionIndex = axis * 2u +
				(direction[axis] >= 0.0f ? 0u : 1u);
			const glm::vec2 moments = probe.m_visibility[directionIndex];
			meanDistance += moments.x * axisWeights[axis];
			meanDistanceSquared += moments.y * axisWeights[axis];
		}
		meanDistance /= totalAxisWeight;
		meanDistanceSquared /= totalAxisWeight;

		const float distanceBias = (std::max)(
			settings.m_normalBias + settings.m_viewBias,
			settings.m_minProbeSpacing * 0.02f);
		const float distanceBeyondMean =
			receiverDistance - distanceBias - meanDistance;
		if (distanceBeyondMean <= 0.0f)
		{
			return 1.0f;
		}

		const float standardDeviationFloor = (std::max)(
			settings.m_minProbeSpacing * 0.02f,
			0.001f);
		const float variance = (std::max)(
			meanDistanceSquared - meanDistance * meanDistance,
			standardDeviationFloor * standardDeviationFloor);
		const float chebyshev = variance /
			(variance + distanceBeyondMean * distanceBeyondMean);
		return chebyshev * chebyshev * chebyshev;
	}
}

glm::vec3 Sailor::EvaluateProbeIrradianceSH(
	const std::array<glm::vec3,
		GIProbeSphericalHarmonicsCoefficientCount>& coefficients,
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
	const float basis[GIProbeSphericalHarmonicsCoefficientCount]
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
		index < GIProbeSphericalHarmonicsCoefficientCount;
		++index)
	{
		irradiance += coefficients[index] * basis[index];
	}
	return glm::max(irradiance, glm::vec3(0.0f));
}

bool Sailor::SampleGIProbesIrradiance(
	const GIProbesData& data,
	const glm::vec3& worldPosition,
	const glm::vec3& worldNormal,
	glm::vec3& outIrradiance,
	GIProbeDebugInfo* outDebugInfo) noexcept
{
	outIrradiance = glm::vec3(0.0f);
	if (outDebugInfo)
	{
		*outDebugInfo = GIProbeDebugInfo{};
	}
	if (data.m_bricks.IsEmpty() || data.m_probes.IsEmpty())
	{
		return false;
	}

	const GIProbeBrick* selectedBrick = nullptr;
	uint32_t selectedBrickIndex = 0u;
	for (uint32_t index = 0u; index < data.m_bricks.Num(); ++index)
	{
		const GIProbeBrick& candidate = data.m_bricks[index];
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
		GIProbeSphericalHarmonicsCoefficientCount> blended{};
	float totalInterpolationWeight = 0.0f;
	float totalVisibleWeight = 0.0f;
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
				const GIProbe& probe = data.m_probes[probeIndex];
				const float trilinearWeight =
					(x ? fraction.x : 1.0f - fraction.x) *
					(y ? fraction.y : 1.0f - fraction.y) *
					(z ? fraction.z : 1.0f - fraction.z);
				const float interpolationWeight = trilinearWeight *
					glm::clamp(probe.m_validity, 0.0f, 1.0f);
					const float surfaceFacingWeight = SurfaceFacingWeight(
						probe,
						worldPosition,
						worldNormal,
						data.m_bakeSettings.m_normalBias);
					const float visibility = surfaceFacingWeight *
						ProbeMomentVisibility(
							probe,
							worldPosition,
							data.m_bakeSettings);
				const float visibleWeight = interpolationWeight * visibility;
				totalInterpolationWeight += interpolationWeight;
				for (uint32_t coefficientIndex = 0u;
					coefficientIndex < GIProbeSphericalHarmonicsCoefficientCount;
					++coefficientIndex)
				{
					blended[coefficientIndex] +=
						probe.m_irradiance[coefficientIndex] * visibleWeight;
				}
				totalVisibleWeight += visibleWeight;
				if (outDebugInfo)
				{
					outDebugInfo->m_probeIndices[cornerIndex] = probeIndex;
					outDebugInfo->m_weights[cornerIndex] = visibleWeight;
				}
			}
		}
	}
	if (!std::isfinite(totalInterpolationWeight) ||
		totalInterpolationWeight <= 1e-6f ||
		!std::isfinite(totalVisibleWeight))
	{
		return false;
	}
	if (totalVisibleWeight <= 1e-6f)
	{
		return false;
	}
	// Never promote a tiny surviving receiver-side weight to a full probe
	// contribution. That amplification turns a single bright probe into white
	// pixels at depth discontinuities and thin alpha-tested geometry. Cells with
	// at least half of their valid interpolation support retain the established
	// normalized result; lower coverage fades conservatively instead.
	const float normalizationWeight = (std::max)(
		totalVisibleWeight,
		totalInterpolationWeight * MinimumReceiverCoverage);
	for (glm::vec3& coefficient : blended)
	{
		coefficient /= normalizationWeight;
	}
	outIrradiance = EvaluateProbeIrradianceSH(blended, worldNormal);
	if (outDebugInfo)
	{
		outDebugInfo->m_brickIndex = selectedBrickIndex;
		outDebugInfo->m_totalUnnormalizedWeight = totalVisibleWeight;
		for (float& weight : outDebugInfo->m_weights)
		{
			weight /= normalizationWeight;
		}
	}
	return true;
}
