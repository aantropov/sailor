#include "AssetRegistry/GlobalIllumination/ProbeVolumeData.h"

#include "Containers/Hash.h"

#include <cmath>
#include <limits>

using namespace Sailor;

namespace
{
	constexpr uint32_t MaxProbeCount = 16u * 1024u * 1024u;
	constexpr uint32_t MaxBrickCount = 1024u * 1024u;

	bool IsFinite(const glm::vec2& value) noexcept
	{
		return std::isfinite(value.x) && std::isfinite(value.y);
	}

	bool IsFinite(const glm::vec3& value) noexcept
	{
		return std::isfinite(value.x) &&
			std::isfinite(value.y) &&
			std::isfinite(value.z);
	}

	void HashBytes(uint64_t& hash, const void* data, size_t size) noexcept
	{
		const uint8_t* bytes = static_cast<const uint8_t*>(data);
		for (size_t index = 0u; index < size; ++index)
		{
			hash ^= bytes[index];
			hash *= 1099511628211ull;
		}
	}

	template<typename T>
	void HashValue(uint64_t& hash, const T& value) noexcept
	{
		HashBytes(hash, &value, sizeof(value));
	}

	void HashVec3(uint64_t& hash, const glm::vec3& value) noexcept
	{
		HashValue(hash, value.x);
		HashValue(hash, value.y);
		HashValue(hash, value.z);
	}
}

uint64_t Sailor::ComputeProbeVolumeRepresentationHash(
	uint32_t formatVersion,
	uint32_t shOrder,
	EProbeVolumeCompression compression) noexcept
{
	uint64_t hash = 1469598103934665603ull;
	HashValue(hash, formatVersion);
	HashValue(hash, shOrder);
	const uint32_t compressionValue = static_cast<uint32_t>(compression);
	HashValue(hash, compressionValue);
	const uint32_t coefficientCount =
		ProbeVolumeSphericalHarmonicsCoefficientCount;
	HashValue(hash, coefficientCount);
	const uint32_t visibilityDirectionCount =
		ProbeVolumeVisibilityDirectionCount;
	HashValue(hash, visibilityDirectionCount);
	return hash;
}

uint64_t Sailor::ComputeProbeVolumeLayoutHash(
	const ProbeVolumeData& data) noexcept
{
	uint64_t hash = 1469598103934665603ull;
	HashVec3(hash, data.m_volumeMin);
	HashVec3(hash, data.m_volumeMax);
	const uint32_t numBricks = static_cast<uint32_t>(data.m_bricks.Num());
	const uint32_t numProbes = static_cast<uint32_t>(data.m_probes.Num());
	HashValue(hash, numBricks);
	HashValue(hash, numProbes);

	for (const ProbeVolumeBrick& brick : data.m_bricks)
	{
		HashVec3(hash, brick.m_min);
		HashVec3(hash, brick.m_max);
		HashValue(hash, brick.m_subdivisionLevel);
		HashValue(hash, brick.m_firstProbeIndex);
		HashValue(hash, brick.m_probeCount);
		HashValue(hash, brick.m_probeCounts.x);
		HashValue(hash, brick.m_probeCounts.y);
		HashValue(hash, brick.m_probeCounts.z);
	}

	for (const ProbeVolumeSample& probe : data.m_probes)
	{
		HashVec3(hash, probe.m_position);
	}

	return hash;
}

bool ProbeVolumeData::Validate(std::string& outDiagnostic) const
{
	outDiagnostic.clear();
	if (m_formatVersion != ProbeVolumeFormatVersion)
	{
		outDiagnostic = "unsupported probe-volume data version";
		return false;
	}
	if (m_shOrder != ProbeVolumeSphericalHarmonicsOrder)
	{
		outDiagnostic = "only order-2 spherical harmonics are supported";
		return false;
	}
	if (m_compression != EProbeVolumeCompression::Float32)
	{
		outDiagnostic = "unsupported probe-volume coefficient compression";
		return false;
	}
	if (m_stateName.empty() || m_bakerVersion.empty())
	{
		outDiagnostic =
			"the probe volume must identify its baked state and baker version";
		return false;
	}
	if (!std::isfinite(m_diagnostics.m_averageValidity) ||
		m_diagnostics.m_averageValidity < 0.0f ||
		m_diagnostics.m_averageValidity > 1.0f ||
		!std::isfinite(m_diagnostics.m_bakeDurationSeconds) ||
		m_diagnostics.m_bakeDurationSeconds < 0.0f)
	{
		outDiagnostic = "the probe volume has invalid bake diagnostics";
		return false;
	}
	if (m_probes.IsEmpty() || m_probes.Num() > MaxProbeCount)
	{
		outDiagnostic = "probe count is zero or exceeds the supported limit";
		return false;
	}
	if (m_bricks.IsEmpty() || m_bricks.Num() > MaxBrickCount)
	{
		outDiagnostic = "brick count is zero or exceeds the supported limit";
		return false;
	}
	if (!IsFinite(m_volumeMin) ||
		!IsFinite(m_volumeMax) ||
		glm::any(glm::lessThanEqual(m_volumeMax, m_volumeMin)))
	{
		outDiagnostic = "the probe volume has invalid bounds";
		return false;
	}
	if (m_bakeSettings.m_raysPerProbe == 0u ||
		m_bakeSettings.m_raysPerProbe > ProbeVolumeMaxRaysPerProbe ||
		m_bakeSettings.m_bounceCount == 0u ||
		m_bakeSettings.m_bounceCount > ProbeVolumeMaxBounceCount ||
		m_bakeSettings.m_maxSubdivisionLevel >
			ProbeVolumeMaxSubdivisionLevel ||
		!std::isfinite(m_bakeSettings.m_minProbeSpacing) ||
		m_bakeSettings.m_minProbeSpacing <= 0.0f ||
		!std::isfinite(m_bakeSettings.m_normalBias) ||
		m_bakeSettings.m_normalBias < 0.0f ||
		!std::isfinite(m_bakeSettings.m_viewBias) ||
		m_bakeSettings.m_viewBias < 0.0f ||
		!std::isfinite(m_bakeSettings.m_maxRayDistance) ||
		m_bakeSettings.m_maxRayDistance <= 0.0f)
	{
		outDiagnostic = "the probe volume has invalid bake settings";
		return false;
	}

	uint64_t coveredProbeCount = 0u;
	for (const ProbeVolumeBrick& brick : m_bricks)
	{
		if (!IsFinite(brick.m_min) ||
			!IsFinite(brick.m_max) ||
			glm::any(glm::lessThanEqual(brick.m_max, brick.m_min)) ||
			glm::any(glm::lessThan(brick.m_min, m_volumeMin)) ||
			glm::any(glm::greaterThan(brick.m_max, m_volumeMax)) ||
			brick.m_subdivisionLevel > m_bakeSettings.m_maxSubdivisionLevel ||
			brick.m_probeCount == 0u ||
			brick.m_probeCounts.x == 0u ||
			brick.m_probeCounts.y == 0u ||
			brick.m_probeCounts.z == 0u)
		{
			outDiagnostic = "the adaptive brick table is invalid or non-contiguous";
			return false;
		}

		const uint64_t probeCountXY =
			static_cast<uint64_t>(brick.m_probeCounts.x) *
			brick.m_probeCounts.y;
		if (probeCountXY >
			(std::numeric_limits<uint64_t>::max)() /
			brick.m_probeCounts.z)
		{
			outDiagnostic = "an adaptive brick probe-grid size overflows the format";
			return false;
		}
		const uint64_t expectedBrickProbeCount =
			probeCountXY * brick.m_probeCounts.z;
		if (expectedBrickProbeCount != brick.m_probeCount ||
			brick.m_firstProbeIndex != coveredProbeCount ||
			static_cast<uint64_t>(brick.m_firstProbeIndex) +
				brick.m_probeCount > m_probes.Num())
		{
			outDiagnostic = "the adaptive brick table is invalid or non-contiguous";
			return false;
		}
		coveredProbeCount += brick.m_probeCount;
	}
	if (coveredProbeCount != m_probes.Num())
	{
		outDiagnostic = "the adaptive brick table does not cover every probe exactly once";
		return false;
	}
	if (m_diagnostics.m_invalidProbeCount > m_probes.Num() ||
		m_diagnostics.m_relocatedProbeCount > m_probes.Num())
	{
		outDiagnostic = "the probe-volume diagnostic counts exceed the probe count";
		return false;
	}

	for (const ProbeVolumeSample& probe : m_probes)
	{
		constexpr uint32_t KnownProbeFlags =
			EProbeVolumeSampleFlag::Valid |
			EProbeVolumeSampleFlag::Relocated;
		if (!IsFinite(probe.m_position) ||
			!IsFinite(probe.m_relocationOffset) ||
			glm::any(glm::lessThan(probe.m_position, m_volumeMin)) ||
			glm::any(glm::greaterThan(probe.m_position, m_volumeMax)) ||
			!std::isfinite(probe.m_validity) ||
			probe.m_validity < 0.0f ||
			probe.m_validity > 1.0f ||
			(probe.m_flags & ~KnownProbeFlags) != 0u)
		{
			outDiagnostic =
				"a probe has invalid position, relocation, validity, or flags";
			return false;
		}
		for (const glm::vec3& coefficient : probe.m_irradiance)
		{
			if (!IsFinite(coefficient))
			{
				outDiagnostic = "a probe has a non-finite irradiance coefficient";
				return false;
			}
		}
		for (const glm::vec2& visibility : probe.m_visibility)
		{
			if (!IsFinite(visibility) ||
				visibility.x < 0.0f ||
				visibility.y < 0.0f)
			{
				outDiagnostic = "a probe has invalid visibility moments";
				return false;
			}
		}
	}

	const uint64_t expectedLayoutHash = ComputeProbeVolumeLayoutHash(*this);
	if (m_layoutHash != 0u && m_layoutHash != expectedLayoutHash)
	{
		outDiagnostic = "the stored layout hash does not match the spatial payload";
		return false;
	}
	const uint64_t expectedRepresentationHash =
		ComputeProbeVolumeRepresentationHash(
			m_formatVersion,
			m_shOrder,
			m_compression);
	if (m_representationHash != 0u &&
		m_representationHash != expectedRepresentationHash)
	{
		outDiagnostic = "the stored representation hash does not match the payload encoding";
		return false;
	}

	return true;
}

bool ProbeVolumeData::IsCompositionCompatibleWith(
	const ProbeVolumeData& rhs,
	std::string& outDiagnostic) const
{
	outDiagnostic.clear();
	const uint64_t lhsRepresentation = m_representationHash != 0u
		? m_representationHash
		: ComputeProbeVolumeRepresentationHash(
			m_formatVersion,
			m_shOrder,
			m_compression);
	const uint64_t rhsRepresentation = rhs.m_representationHash != 0u
		? rhs.m_representationHash
		: ComputeProbeVolumeRepresentationHash(
			rhs.m_formatVersion,
			rhs.m_shOrder,
			rhs.m_compression);
	if (lhsRepresentation != rhsRepresentation)
	{
		outDiagnostic = "probe representation hashes differ";
		return false;
	}

	const uint64_t lhsLayout = m_layoutHash != 0u
		? m_layoutHash
		: ComputeProbeVolumeLayoutHash(*this);
	const uint64_t rhsLayout = rhs.m_layoutHash != 0u
		? rhs.m_layoutHash
		: ComputeProbeVolumeLayoutHash(rhs);
	if (lhsLayout != rhsLayout)
	{
		outDiagnostic = "probe layout hashes differ";
		return false;
	}
	if (m_transportHash == 0u || rhs.m_transportHash == 0u)
	{
		outDiagnostic = "probe transport hash is missing";
		return false;
	}
	if (m_transportHash != rhs.m_transportHash)
	{
		outDiagnostic = "probe transport and visibility hashes differ";
		return false;
	}
	return true;
}
