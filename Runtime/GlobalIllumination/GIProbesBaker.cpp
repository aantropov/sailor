#include "GlobalIllumination/GIProbesBaker.h"

#include "Containers/Hash.h"
#include "Core/Utils.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

using namespace Sailor;

namespace
{
	constexpr float Pi = 3.14159265358979323846f;
	constexpr uint32_t MaxBakeBrickCount = 1024u * 1024u;
	constexpr uint32_t MaxBakeProbeCount = 16u * 1024u * 1024u;
	constexpr uint32_t ProbeTransportRayCount = 64u;
	constexpr uint32_t MaxProbeRelocationIterations = 4u;
	constexpr float MaxValidBackfaceRatio = 0.25f;

	const std::array<glm::vec3, GIProbeVisibilityDirectionCount>
		VisibilityDirections
	{
		glm::vec3(1.0f, 0.0f, 0.0f),
		glm::vec3(-1.0f, 0.0f, 0.0f),
		glm::vec3(0.0f, 1.0f, 0.0f),
		glm::vec3(0.0f, -1.0f, 0.0f),
		glm::vec3(0.0f, 0.0f, 1.0f),
		glm::vec3(0.0f, 0.0f, -1.0f)
	};

	bool IsFinite(const glm::vec3& value) noexcept
	{
		return std::isfinite(value.x) &&
			std::isfinite(value.y) &&
			std::isfinite(value.z);
	}

	bool IsCancelled(const GIProbesBakeRequest& request) noexcept
	{
		return request.m_cancel &&
			request.m_cancel->load(std::memory_order_acquire);
	}

	uint32_t MixRandomSeed(
		uint32_t baseSeed,
		uint32_t probeIndex,
		uint32_t sampleIndex,
		uint32_t stream) noexcept
	{
		uint32_t value = baseSeed ^ 0x9e3779b9u;
		value ^= probeIndex * 0x85ebca6bu + 0xc2b2ae35u;
		value ^= sampleIndex * 0x27d4eb2du + stream * 0x165667b1u;
		value ^= value >> 16u;
		value *= 0x7feb352du;
		value ^= value >> 15u;
		value *= 0x846ca68bu;
		value ^= value >> 16u;
		return value != 0u ? value : 0x6d2b79f5u;
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

	void HashVec2(uint64_t& hash, const glm::vec2& value) noexcept
	{
		HashValue(hash, value.x);
		HashValue(hash, value.y);
	}

	void HashVec3(uint64_t& hash, const glm::vec3& value) noexcept
	{
		HashValue(hash, value.x);
		HashValue(hash, value.y);
		HashValue(hash, value.z);
	}

	uint64_t ComputeTransportHash(const GIProbesData& data) noexcept
	{
		uint64_t hash = 1469598103934665603ull;
		HashValue(hash, data.m_layoutHash);
		HashValue(hash, data.m_bakeSettings.m_maxSubdivisionLevel);
		HashValue(hash, data.m_bakeSettings.m_minProbeSpacing);
		HashValue(hash, data.m_bakeSettings.m_normalBias);
		HashValue(hash, data.m_bakeSettings.m_viewBias);
		HashValue(hash, data.m_bakeSettings.m_maxRayDistance);
		for (const GIProbe& probe : data.m_probes)
		{
			HashVec3(hash, probe.m_relocationOffset);
			HashValue(hash, probe.m_validity);
			HashValue(hash, probe.m_flags);
			for (const glm::vec2& moments : probe.m_visibility)
			{
				HashVec2(hash, moments);
			}
			for (const float environmentVisibility :
				probe.m_environmentVisibility)
			{
				HashValue(hash, environmentVisibility);
			}
		}
		return hash;
	}

	uint64_t ComputeLightingHash(const GIProbesData& data) noexcept
	{
		uint64_t hash = 1469598103934665603ull;
		HashValue(hash, data.m_layoutHash);
		HashValue(hash, data.m_bakeSettings.m_raysPerProbe);
		HashValue(hash, data.m_bakeSettings.m_bounceCount);
		HashValue(hash, data.m_bakeSettings.m_randomSeed);
		HashValue(hash, data.m_bakeSettings.m_skyIndirectIntensity);
		for (const GIProbe& probe : data.m_probes)
		{
			for (const glm::vec3& coefficient : probe.m_irradiance)
			{
				HashVec3(hash, coefficient);
			}
		}
		return hash;
	}

	uint32_t CanonicalFloatBits(float value) noexcept
	{
		return value == 0.0f ? 0u : std::bit_cast<uint32_t>(value);
	}

	struct SharedProbeEntry final
	{
		std::array<uint32_t, 3u> m_positionBits{};
		uint32_t m_subdivisionLevel = 0u;
		uint32_t m_probeIndex = 0u;
	};

	bool SharedProbeEntryLess(
		const SharedProbeEntry& lhs,
		const SharedProbeEntry& rhs) noexcept
	{
		for (size_t axis = 0u; axis < lhs.m_positionBits.size(); ++axis)
		{
			if (lhs.m_positionBits[axis] != rhs.m_positionBits[axis])
			{
				return lhs.m_positionBits[axis] < rhs.m_positionBits[axis];
			}
		}
		if (lhs.m_subdivisionLevel != rhs.m_subdivisionLevel)
		{
			return lhs.m_subdivisionLevel < rhs.m_subdivisionLevel;
		}
		return lhs.m_probeIndex < rhs.m_probeIndex;
	}

	bool HasSameNominalPosition(
		const SharedProbeEntry& lhs,
		const SharedProbeEntry& rhs) noexcept
	{
		return lhs.m_positionBits == rhs.m_positionBits;
	}

	bool HasSameTransportSupport(
		const SharedProbeEntry& lhs,
		const SharedProbeEntry& rhs) noexcept
	{
		return HasSameNominalPosition(lhs, rhs) &&
			lhs.m_subdivisionLevel == rhs.m_subdivisionLevel;
	}

	void CanonicalizeSharedProbeSamples(
		GIProbesData& data,
		bool bCanonicalizeTransport)
	{
		std::vector<SharedProbeEntry> entries;
		entries.reserve(data.m_probes.Num());
		for (const GIProbeBrick& brick : data.m_bricks)
		{
			for (uint32_t z = 0u; z < brick.m_probeCounts.z; ++z)
			{
				for (uint32_t y = 0u; y < brick.m_probeCounts.y; ++y)
				{
					for (uint32_t x = 0u; x < brick.m_probeCounts.x; ++x)
					{
						const uint32_t probeIndex = brick.m_firstProbeIndex + x +
							brick.m_probeCounts.x *
							(y + brick.m_probeCounts.y * z);
						if (probeIndex >= data.m_probes.Num())
						{
							continue;
						}
						const glm::vec3 fraction(
							brick.m_probeCounts.x > 1u ?
								static_cast<float>(x) /
								static_cast<float>(brick.m_probeCounts.x - 1u) : 0.0f,
							brick.m_probeCounts.y > 1u ?
								static_cast<float>(y) /
								static_cast<float>(brick.m_probeCounts.y - 1u) : 0.0f,
							brick.m_probeCounts.z > 1u ?
								static_cast<float>(z) /
								static_cast<float>(brick.m_probeCounts.z - 1u) : 0.0f);
						const glm::vec3 nominalPosition = glm::mix(
							brick.m_min,
							brick.m_max,
							fraction);
						entries.push_back({
							{
								CanonicalFloatBits(nominalPosition.x),
								CanonicalFloatBits(nominalPosition.y),
								CanonicalFloatBits(nominalPosition.z)
							},
							brick.m_subdivisionLevel,
							probeIndex });
					}
				}
			}
		}
		std::sort(entries.begin(), entries.end(), SharedProbeEntryLess);

		for (size_t begin = 0u; begin < entries.size();)
		{
			size_t end = begin + 1u;
			while (end < entries.size() &&
				HasSameNominalPosition(entries[begin], entries[end]))
			{
				++end;
			}
			const size_t sampleCount = end - begin;
			if (sampleCount > 1u)
			{
				std::array<glm::dvec3,
					GIProbeSphericalHarmonicsCoefficientCount> irradiance{};
				for (size_t entryIndex = begin; entryIndex < end; ++entryIndex)
				{
					const GIProbe& probe =
						data.m_probes[entries[entryIndex].m_probeIndex];
					for (size_t coefficientIndex = 0u;
						coefficientIndex < irradiance.size();
						++coefficientIndex)
					{
						irradiance[coefficientIndex] +=
							glm::dvec3(probe.m_irradiance[coefficientIndex]);
					}
				}

				const double inverseSampleCount =
					1.0 / static_cast<double>(sampleCount);
				std::array<glm::vec3,
					GIProbeSphericalHarmonicsCoefficientCount>
					canonicalIrradiance{};
				for (size_t coefficientIndex = 0u;
					coefficientIndex < irradiance.size();
					++coefficientIndex)
				{
					canonicalIrradiance[coefficientIndex] = glm::vec3(
						irradiance[coefficientIndex] * inverseSampleCount);
				}
				if (!bCanonicalizeTransport)
				{
					for (size_t entryIndex = begin; entryIndex < end; ++entryIndex)
					{
						data.m_probes[entries[entryIndex].m_probeIndex].m_irradiance =
							canonicalIrradiance;
					}
					begin = end;
					continue;
				}

				// A corner shared by different adaptive levels has different local
				// support. Its radiance estimate is common, but averaging its locally
				// clamped transport would make both levels incorrect.
				for (size_t transportBegin = begin;
					transportBegin < end;)
				{
					size_t transportEnd = transportBegin + 1u;
					while (transportEnd < end && HasSameTransportSupport(
						entries[transportBegin],
						entries[transportEnd]))
					{
						++transportEnd;
					}
					const size_t transportSampleCount =
						transportEnd - transportBegin;
					glm::dvec3 position{};
					glm::dvec3 relocation{};
					double validity = 0.0;
					std::array<glm::dvec2,
						GIProbeVisibilityDirectionCount> visibility{};
					std::array<double,
						GIProbeVisibilityDirectionCount>
						environmentVisibility{};
					uint32_t flags = 0u;
					for (size_t entryIndex = transportBegin;
						entryIndex < transportEnd;
						++entryIndex)
					{
						const GIProbe& probe =
							data.m_probes[entries[entryIndex].m_probeIndex];
						position += glm::dvec3(probe.m_position);
						relocation += glm::dvec3(probe.m_relocationOffset);
						validity += static_cast<double>(probe.m_validity);
						flags |= probe.m_flags;
						for (size_t directionIndex = 0u;
							directionIndex < visibility.size();
							++directionIndex)
						{
							visibility[directionIndex] +=
								glm::dvec2(probe.m_visibility[directionIndex]);
							environmentVisibility[directionIndex] +=
								static_cast<double>(
									probe.m_environmentVisibility[directionIndex]);
						}
					}

					const double inverseTransportSampleCount =
						1.0 / static_cast<double>(transportSampleCount);
					GIProbe canonical =
						data.m_probes[entries[transportBegin].m_probeIndex];
					canonical.m_irradiance = canonicalIrradiance;
					canonical.m_position = glm::vec3(
						position * inverseTransportSampleCount);
					canonical.m_relocationOffset = glm::vec3(
						relocation * inverseTransportSampleCount);
					canonical.m_validity = static_cast<float>(
						validity * inverseTransportSampleCount);
					for (size_t directionIndex = 0u;
						directionIndex < visibility.size();
						++directionIndex)
					{
						canonical.m_visibility[directionIndex] = glm::vec2(
							visibility[directionIndex] * inverseTransportSampleCount);
						canonical.m_environmentVisibility[directionIndex] =
							static_cast<float>(
								environmentVisibility[directionIndex] *
								inverseTransportSampleCount);
					}
					const uint32_t validityAndRelocationFlags =
						static_cast<uint32_t>(EGIProbeFlag::Valid) |
						static_cast<uint32_t>(EGIProbeFlag::Relocated);
					canonical.m_flags = flags & ~validityAndRelocationFlags;
					if (canonical.m_validity > 0.05f)
					{
						canonical.m_flags |= static_cast<uint32_t>(
							EGIProbeFlag::Valid);
					}
					if (glm::length(canonical.m_relocationOffset) > 1e-6f)
					{
						canonical.m_flags |= static_cast<uint32_t>(
							EGIProbeFlag::Relocated);
					}
					for (size_t entryIndex = transportBegin;
						entryIndex < transportEnd;
						++entryIndex)
					{
						data.m_probes[entries[entryIndex].m_probeIndex] = canonical;
					}
					transportBegin = transportEnd;
				}
			}
			begin = end;
		}
	}

	bool IntersectsGeometryNeighborhood(
		const glm::vec3& min,
		const glm::vec3& max,
		float margin,
		const TVector<Math::AABB>& geometryBounds) noexcept
	{
		const glm::vec3 expansion((std::max)(margin, 0.0f));
		for (const Math::AABB& bounds : geometryBounds)
		{
			if (bounds.IsValid() &&
				glm::all(glm::lessThanEqual(min, bounds.m_max + expansion)) &&
				glm::all(glm::greaterThanEqual(max, bounds.m_min - expansion)))
			{
				return true;
			}
		}
		return false;
	}

	bool AppendBrick(
		GIProbesData& data,
		const glm::vec3& min,
		const glm::vec3& max,
		uint32_t subdivisionLevel,
		std::string& outDiagnostic)
	{
		if (data.m_bricks.Num() >= MaxBakeBrickCount ||
			data.m_probes.Num() > MaxBakeProbeCount - 8u)
		{
			outDiagnostic = "adaptive probe layout exceeds the supported bake limits";
			return false;
		}

		GIProbeBrick brick;
		brick.m_min = min;
		brick.m_max = max;
		brick.m_subdivisionLevel = subdivisionLevel;
		brick.m_firstProbeIndex = static_cast<uint32_t>(data.m_probes.Num());
		brick.m_probeCounts = glm::uvec3(2u);
		brick.m_probeCount = 8u;
		data.m_bricks.Add(brick);

		for (uint32_t z = 0u; z < 2u; ++z)
		{
			for (uint32_t y = 0u; y < 2u; ++y)
			{
				for (uint32_t x = 0u; x < 2u; ++x)
				{
					GIProbe probe;
					const glm::vec3 fraction(
						static_cast<float>(x),
						static_cast<float>(y),
						static_cast<float>(z));
					probe.m_position = glm::mix(min, max, fraction);
					data.m_probes.Add(std::move(probe));
				}
			}
		}
		return true;
	}

	bool BuildAdaptiveLayout(
		const GIProbesBakeRequest& request,
		GIProbesData& data,
		std::string& outDiagnostic)
	{
		if (request.m_layoutSource)
		{
			if (request.m_layoutSource->m_bakerVersion != request.m_bakerVersion)
			{
				outDiagnostic =
					"layout source transport was produced by a different baker "
					"version; use Bake New before reusing its layout";
				return false;
			}
			std::string sourceDiagnostic;
			if (!request.m_layoutSource->Validate(sourceDiagnostic))
			{
				outDiagnostic = "layout source is invalid: " + sourceDiagnostic;
				return false;
			}
			data.m_volumeMin = request.m_layoutSource->m_volumeMin;
			data.m_volumeMax = request.m_layoutSource->m_volumeMax;
			data.m_bakeSettings.m_maxSubdivisionLevel =
				request.m_layoutSource->m_bakeSettings.m_maxSubdivisionLevel;
			data.m_bakeSettings.m_minProbeSpacing =
				request.m_layoutSource->m_bakeSettings.m_minProbeSpacing;
			data.m_bakeSettings.m_normalBias =
				request.m_layoutSource->m_bakeSettings.m_normalBias;
			data.m_bakeSettings.m_viewBias =
				request.m_layoutSource->m_bakeSettings.m_viewBias;
			data.m_bakeSettings.m_maxRayDistance =
				request.m_layoutSource->m_bakeSettings.m_maxRayDistance;
			data.m_bricks = request.m_layoutSource->m_bricks;
			data.m_probes = request.m_layoutSource->m_probes;
			for (GIProbe& probe : data.m_probes)
			{
				probe.m_irradiance = {};
			}
			data.m_layoutHash = request.m_layoutSource->m_layoutHash;
			data.m_transportHash = request.m_layoutSource->m_transportHash;
			return true;
		}

		data.m_volumeMin = request.m_volumeMin;
		data.m_volumeMax = request.m_volumeMax;
		std::function<bool(const glm::vec3&, const glm::vec3&, uint32_t)>
			appendNode;
		appendNode = [&](const glm::vec3& min,
			const glm::vec3& max,
			uint32_t subdivisionLevel) -> bool
		{
			const glm::vec3 childExtent = (max - min) * 0.5f;
			const glm::bvec3 splitAxes =
				subdivisionLevel < request.m_settings.m_maxSubdivisionLevel ?
					glm::greaterThanEqual(
						childExtent,
						glm::vec3(request.m_settings.m_minProbeSpacing)) :
					glm::bvec3(false);
			// Keep one finest-spacing shell of probes around geometry. Testing only
			// the exact renderer bounds leaves the first probe row outside a wall at
			// the coarse parent spacing, which is precisely where interpolation needs
			// enough samples to distinguish an open side from an occluded side.
			const bool bShouldSubdivide = glm::any(splitAxes) &&
				IntersectsGeometryNeighborhood(
					min,
					max,
					request.m_settings.m_minProbeSpacing,
					request.m_sceneGeometryBounds);
			if (!bShouldSubdivide)
			{
				return AppendBrick(
					data,
					min,
					max,
					subdivisionLevel,
					outDiagnostic);
			}

			const glm::vec3 center = (min + max) * 0.5f;
			const glm::uvec3 childCounts(
				splitAxes.x ? 2u : 1u,
				splitAxes.y ? 2u : 1u,
				splitAxes.z ? 2u : 1u);
			for (uint32_t z = 0u; z < childCounts.z; ++z)
			{
				for (uint32_t y = 0u; y < childCounts.y; ++y)
				{
					for (uint32_t x = 0u; x < childCounts.x; ++x)
					{
						const glm::bvec3 upper(x != 0u, y != 0u, z != 0u);
						const glm::vec3 childMin(
							splitAxes.x && upper.x ? center.x : min.x,
							splitAxes.y && upper.y ? center.y : min.y,
							splitAxes.z && upper.z ? center.z : min.z);
						const glm::vec3 childMax(
							!splitAxes.x || upper.x ? max.x : center.x,
							!splitAxes.y || upper.y ? max.y : center.y,
							!splitAxes.z || upper.z ? max.z : center.z);
						if (!appendNode(
							childMin,
							childMax,
							subdivisionLevel + 1u))
						{
							return false;
						}
					}
				}
			}
			return true;
		};

		if (!appendNode(data.m_volumeMin, data.m_volumeMax, 0u))
		{
			return false;
		}
		data.m_layoutHash = ComputeGIProbesLayoutHash(data);
		return true;
	}

	uint32_t CalculateRequiredSubdivisionLevel(
		glm::vec3 extent,
		float minProbeSpacing) noexcept
	{
		uint32_t level = 0u;
		for (; level < GIProbesMaxSubdivisionLevel; ++level)
		{
			const glm::bvec3 splitAxes = glm::greaterThanEqual(
				extent * 0.5f,
				glm::vec3(minProbeSpacing));
			if (!glm::any(splitAxes))
			{
				return level;
			}
			if (splitAxes.x) extent.x *= 0.5f;
			if (splitAxes.y) extent.y *= 0.5f;
			if (splitAxes.z) extent.z *= 0.5f;
		}

		return glm::any(glm::greaterThanEqual(
			extent * 0.5f,
			glm::vec3(minProbeSpacing))) ?
			GIProbesMaxSubdivisionLevel + 1u : level;
	}

	std::vector<float> CalculateProbeVisibilityMaxDistances(
		const GIProbesData& data)
	{
		std::vector<float> result(
			data.m_probes.Num(),
			data.m_bakeSettings.m_maxRayDistance);
		for (const GIProbeBrick& brick : data.m_bricks)
		{
			const float maxDistance = CalculateGIProbeVisibilityMaxDistance(
				data,
				brick);
			const uint32_t endProbeIndex = (std::min)(
				brick.m_firstProbeIndex + brick.m_probeCount,
				static_cast<uint32_t>(result.size()));
			for (uint32_t probeIndex = brick.m_firstProbeIndex;
				probeIndex < endProbeIndex;
				++probeIndex)
			{
				result[probeIndex] = maxDistance;
			}
		}
		return result;
	}

	glm::vec3 FibonacciDirection(
		uint32_t index,
		uint32_t count,
		uint32_t seed,
		uint32_t probeIndex) noexcept
	{
		const float offset = 2.0f / static_cast<float>(count);
		const float y = static_cast<float>(index) * offset - 1.0f +
			offset * 0.5f;
		const float radius = std::sqrt((std::max)(0.0f, 1.0f - y * y));
		const uint32_t rotationBits =
			seed * 747796405u + probeIndex * 2891336453u;
		const float rotation = static_cast<float>(rotationBits & 0x00ffffffu) /
			16777216.0f * 2.0f * Pi;
		const float phi = static_cast<float>(index) * 2.39996322972865332f +
			rotation;
		return glm::vec3(
			std::cos(phi) * radius,
			y,
			std::sin(phi) * radius);
	}

	void EvaluateSphericalHarmonicsBasis(
		const glm::vec3& direction,
		float* outBasis) noexcept
	{
		const float x = direction.x;
		const float y = direction.y;
		const float z = direction.z;
		outBasis[0] = 0.2820947918f;
		outBasis[1] = 0.4886025119f * y;
		outBasis[2] = 0.4886025119f * z;
		outBasis[3] = 0.4886025119f * x;
		outBasis[4] = 1.0925484306f * x * y;
		outBasis[5] = 1.0925484306f * y * z;
		outBasis[6] = 0.3153915653f * (3.0f * z * z - 1.0f);
		outBasis[7] = 1.0925484306f * x * z;
		outBasis[8] = 0.5462742153f * (x * x - y * y);
	}

	float IrradianceConvolution(uint32_t coefficientIndex) noexcept
	{
		// Match ComputeIrradianceMap.shader: store diffuse exitant radiance for
		// a white Lambertian surface, including the BRDF's 1 / PI factor.
		return coefficientIndex == 0u ? 1.0f :
			coefficientIndex <= 3u ? 2.0f / 3.0f : 0.25f;
	}

	bool SampleVisibility(
		const IGIProbeBakeRaySampler& sampler,
		const glm::vec3& position,
		float maxDistance,
		uint32_t baseSeed,
		uint32_t probeIndex,
		uint32_t stream,
		std::array<float, GIProbeVisibilityDirectionCount>& outDistances,
		std::string& outDiagnostic)
	{
		for (uint32_t directionIndex = 0u;
			directionIndex < GIProbeVisibilityDirectionCount;
			++directionIndex)
		{
			GIProbeBakeRaySample sample;
			if (!sampler.SampleVisibility(
					position,
					VisibilityDirections[directionIndex],
					maxDistance,
					MixRandomSeed(
						baseSeed,
						probeIndex,
						directionIndex,
						stream),
					sample,
					outDiagnostic))
			{
				return false;
			}
			outDistances[directionIndex] = sample.m_bHit ?
				glm::clamp(sample.m_distance, 0.0f, maxDistance) : maxDistance;
		}
		return true;
	}

	glm::vec3 CalculateRelocation(
		const std::array<float, GIProbeVisibilityDirectionCount>& distances,
		float spacing) noexcept
	{
		const float targetClearance = (std::max)(spacing * 0.25f, 0.001f);
		glm::vec3 relocation{};
		for (uint32_t axis = 0u; axis < 3u; ++axis)
		{
			const float positivePush =
				(std::max)(0.0f, targetClearance - distances[axis * 2u]);
			const float negativePush =
				(std::max)(0.0f, targetClearance - distances[axis * 2u + 1u]);
			relocation[axis] = negativePush - positivePush;
		}
		const float maxRelocation = spacing * 0.45f;
		const float length = glm::length(relocation);
		return length > maxRelocation && length > 1e-6f ?
			relocation * (maxRelocation / length) : relocation;
	}

	struct ProbeTransport final
	{
		std::array<double, GIProbeVisibilityDirectionCount>
			m_weights{};
		std::array<double, GIProbeVisibilityDirectionCount>
			m_environmentVisibilitySums{};
		std::array<double, GIProbeVisibilityDirectionCount>
			m_distanceSums{};
		std::array<double, GIProbeVisibilityDirectionCount>
			m_distanceSquaredSums{};
		uint32_t m_backfaceCount = 0u;
		float m_closestBackfaceDistance =
			(std::numeric_limits<float>::max)();
		glm::vec3 m_closestBackfaceDirection{};
		glm::vec3 m_frontFaceRepulsion{};
		float m_frontFaceClearanceDeficit = 0.0f;
	};

	glm::vec3 CalculateWallRepulsion(
		const ProbeTransport& transport) noexcept
	{
		const float directionLength = glm::length(
			transport.m_frontFaceRepulsion);
		if (!std::isfinite(directionLength) ||
			directionLength <= 1e-6f ||
			transport.m_frontFaceClearanceDeficit <= 1e-6f)
		{
			return glm::vec3(0.0f);
		}
		return transport.m_frontFaceRepulsion /
			directionLength * transport.m_frontFaceClearanceDeficit;
	}

	bool SampleProbeTransport(
		const GIProbesBakeRequest& request,
		const IGIProbeBakeRaySampler& sampler,
		const glm::vec3& position,
		float targetClearance,
		float visibilityMaxDistance,
		uint32_t probeIndex,
		uint32_t stream,
		ProbeTransport& outTransport,
		std::string& outDiagnostic)
	{
		outTransport = {};
		for (uint32_t rayIndex = 0u;
			rayIndex < ProbeTransportRayCount;
			++rayIndex)
		{
			if (IsCancelled(request))
			{
				outDiagnostic = "GI probe bake was cancelled";
				return false;
			}

			// Transport directions deliberately do not rotate per probe. The
			// radiance estimator remains decorrelated below, while fixed transport
			// prevents random visibility islands from moving between neighbouring
			// probes and duplicate brick corners.
			const glm::vec3 direction = FibonacciDirection(
				rayIndex,
				ProbeTransportRayCount,
				0u,
				0u);
			GIProbeBakeRaySample sample;
			if (!sampler.SampleVisibility(
					position,
					direction,
					request.m_settings.m_maxRayDistance,
					MixRandomSeed(
						request.m_settings.m_randomSeed,
						probeIndex,
						rayIndex,
						stream),
					sample,
					outDiagnostic))
			{
				return false;
			}

			const glm::vec3 axisWeights = glm::abs(direction);
			const float localDistance = sample.m_bHit &&
				std::isfinite(sample.m_distance) ?
				glm::clamp(sample.m_distance, 0.0f, visibilityMaxDistance) :
				visibilityMaxDistance;
			for (uint32_t axis = 0u; axis < 3u; ++axis)
			{
				const uint32_t directionIndex = axis * 2u +
					(direction[axis] >= 0.0f ? 0u : 1u);
				const double weight = static_cast<double>(axisWeights[axis]);
				outTransport.m_weights[directionIndex] += weight;
				outTransport.m_environmentVisibilitySums[directionIndex] +=
					weight * (sample.m_bHit ? 0.0 : 1.0);
				outTransport.m_distanceSums[directionIndex] +=
					weight * static_cast<double>(localDistance);
				outTransport.m_distanceSquaredSums[directionIndex] +=
					weight * static_cast<double>(localDistance * localDistance);
			}

			if (sample.m_bHit && sample.m_bBackFace)
			{
				++outTransport.m_backfaceCount;
				if (sample.m_distance <
					outTransport.m_closestBackfaceDistance)
				{
					outTransport.m_closestBackfaceDistance =
						sample.m_distance;
					outTransport.m_closestBackfaceDirection = direction;
				}
			}
			else if (sample.m_bHit &&
				std::isfinite(sample.m_distance) &&
				sample.m_distance < targetClearance)
			{
				const float clearanceDeficit =
					targetClearance - (std::max)(sample.m_distance, 0.0f);
				// A ray points from the probe towards the nearby surface, so its
				// opposite direction is a local estimate of "away from the wall".
				// Combining the fixed spherical directions also resolves corners and
				// slanted walls that the six signed-axis clearance rays cannot see.
				outTransport.m_frontFaceRepulsion -=
					direction * clearanceDeficit;
				outTransport.m_frontFaceClearanceDeficit = (std::max)(
					outTransport.m_frontFaceClearanceDeficit,
					clearanceDeficit);
			}
		}
		return true;
	}

	bool IsEmbeddedProbe(const ProbeTransport& transport) noexcept
	{
		return static_cast<float>(transport.m_backfaceCount) /
			static_cast<float>(ProbeTransportRayCount) >
			MaxValidBackfaceRatio;
	}

	void StoreProbeVisibility(
		const ProbeTransport& transport,
		float fallbackDistance,
		GIProbe& probe) noexcept
	{
		probe.m_flags &= ~GIProbeBlockedDirectionMask;
		const float blockingEpsilon = (std::max)(
			fallbackDistance * 0.0001f,
			0.0001f);
		for (uint32_t directionIndex = 0u;
			directionIndex < GIProbeVisibilityDirectionCount;
			++directionIndex)
		{
			const double weight = transport.m_weights[directionIndex];
			const float meanDistance = weight > 0.0 ?
				static_cast<float>(
					transport.m_distanceSums[directionIndex] / weight) :
				fallbackDistance;
			const float meanDistanceSquared = weight > 0.0 ?
				static_cast<float>(
					transport.m_distanceSquaredSums[directionIndex] / weight) :
				fallbackDistance * fallbackDistance;
			probe.m_visibility[directionIndex] = glm::vec2(
				meanDistance,
				(std::max)(
					meanDistanceSquared,
					meanDistance * meanDistance));
			if (meanDistance < fallbackDistance - blockingEpsilon)
			{
				probe.m_flags |= GIProbeBlockedDirectionBit(
					directionIndex);
			}

			if (weight > 0.0)
			{
				probe.m_environmentVisibility[directionIndex] =
					static_cast<float>(
						transport.m_environmentVisibilitySums[directionIndex] /
							weight);
			}
			else
			{
				probe.m_environmentVisibility[directionIndex] = 1.0f;
			}
		}
	}

	bool BakeProbe(
		const GIProbesBakeRequest& request,
		const IGIProbeBakeRaySampler& sampler,
		uint32_t probeIndex,
		float visibilityMaxDistance,
		bool bReuseTransport,
		GIProbe& probe,
		std::string& outDiagnostic)
	{
		std::array<float, GIProbeVisibilityDirectionCount>
			clearanceDistances{};
		if (!bReuseTransport)
		{
			const glm::vec3 originalPosition = probe.m_position;
			if (!SampleVisibility(
					sampler,
					probe.m_position,
					request.m_settings.m_maxRayDistance,
					request.m_settings.m_randomSeed,
					probeIndex,
					0u,
					clearanceDistances,
					outDiagnostic))
			{
				return false;
			}

			probe.m_relocationOffset = CalculateRelocation(
				clearanceDistances,
				request.m_settings.m_minProbeSpacing);
			if (glm::length(probe.m_relocationOffset) > 1e-6f)
			{
				probe.m_position = glm::clamp(
					originalPosition + probe.m_relocationOffset,
					request.m_volumeMin,
					request.m_volumeMax);
				probe.m_relocationOffset = probe.m_position - originalPosition;
				if (glm::length(probe.m_relocationOffset) > 1e-6f)
				{
					probe.m_flags |= static_cast<uint32_t>(
						EGIProbeFlag::Relocated);
				}
			}

			const float targetClearance = (std::max)(
				request.m_settings.m_minProbeSpacing * 0.25f,
				0.001f);
			const float maxRelocation =
				request.m_settings.m_minProbeSpacing * 0.45f;
			ProbeTransport transport;
			if (!SampleProbeTransport(
					request,
					sampler,
					probe.m_position,
					targetClearance,
					visibilityMaxDistance,
					probeIndex,
					1u,
					transport,
					outDiagnostic))
			{
				return false;
			}

			for (uint32_t iteration = 0u;
				iteration < MaxProbeRelocationIterations;
				++iteration)
			{
				glm::vec3 relocationStep{};
				if (IsEmbeddedProbe(transport))
				{
					if (!std::isfinite(
							transport.m_closestBackfaceDistance) ||
						transport.m_closestBackfaceDistance >=
							(std::numeric_limits<float>::max)() ||
						glm::length(
							transport.m_closestBackfaceDirection) <= 1e-6f)
					{
						break;
					}
					relocationStep =
						transport.m_closestBackfaceDirection *
						(transport.m_closestBackfaceDistance +
							targetClearance);
				}
				else
				{
					relocationStep = CalculateWallRepulsion(transport);
					if (glm::length(relocationStep) <= 1e-6f)
					{
						break;
					}
				}

				glm::vec3 requestedOffset =
					probe.m_position - originalPosition + relocationStep;
				const float requestedLength = glm::length(requestedOffset);
				if (requestedLength > maxRelocation &&
					requestedLength > 1e-6f)
				{
					requestedOffset *= maxRelocation / requestedLength;
				}
				const glm::vec3 relocatedPosition = glm::clamp(
					originalPosition + requestedOffset,
					request.m_volumeMin,
					request.m_volumeMax);
				if (glm::length(relocatedPosition - probe.m_position) <= 1e-6f)
				{
					break;
				}
				probe.m_position = relocatedPosition;
				probe.m_relocationOffset =
					probe.m_position - originalPosition;
				probe.m_flags |= static_cast<uint32_t>(
					EGIProbeFlag::Relocated);
				if (!SampleProbeTransport(
						request,
						sampler,
						probe.m_position,
						targetClearance,
						visibilityMaxDistance,
						probeIndex,
						2u + iteration,
						transport,
						outDiagnostic))
				{
					return false;
				}
			}

			probe.m_validity = IsEmbeddedProbe(transport) ? 0.0f : 1.0f;
			if (probe.m_validity > 0.05f)
			{
				probe.m_flags |= static_cast<uint32_t>(
					EGIProbeFlag::Valid);
			}
			else
			{
				probe.m_flags &= ~static_cast<uint32_t>(
					EGIProbeFlag::Valid);
			}
			StoreProbeVisibility(
				transport,
				visibilityMaxDistance,
				probe);
		}

		probe.m_irradiance = {};
		if (probe.m_validity <= 0.05f)
		{
			return true;
		}
		const uint32_t rayCount = request.m_settings.m_raysPerProbe;
		for (uint32_t rayIndex = 0u; rayIndex < rayCount; ++rayIndex)
		{
			if (IsCancelled(request))
			{
				outDiagnostic = "GI probe bake was cancelled";
				return false;
			}
			// Direction samples are shared by neighbouring probes. The path tracer
			// may replace part of this uniform sequence with HDR environment samples;
			// the returned mixture PDF keeps the SH projection unbiased.
			const glm::vec3 uniformDirection = FibonacciDirection(
				rayIndex,
				rayCount,
				request.m_settings.m_randomSeed,
				0u);
			glm::vec3 direction{};
			float directionPdf = 0.0f;
			if (!sampler.SamplePrimaryDirection(
					uniformDirection,
					rayIndex,
					rayCount,
					MixRandomSeed(
						request.m_settings.m_randomSeed,
						0u,
						rayIndex,
						3u),
					direction,
					directionPdf,
					outDiagnostic))
			{
				return false;
			}
			const float directionLength = glm::length(direction);
			if (!IsFinite(direction) ||
				!std::isfinite(directionLength) ||
				directionLength <= 1e-6f ||
				!std::isfinite(directionPdf) ||
				directionPdf <= 0.0f)
			{
				outDiagnostic =
					"GI probe sampler returned an invalid primary direction or PDF";
				return false;
			}
			direction /= directionLength;
			GIProbeBakeRaySample sample;
			if (!sampler.Sample(
					probe.m_position,
					direction,
					request.m_settings.m_maxRayDistance,
					MixRandomSeed(
						request.m_settings.m_randomSeed,
						probeIndex,
						rayIndex,
						2u),
					sample,
					outDiagnostic))
			{
				return false;
			}
			const glm::vec3 radiance = IsFinite(sample.m_radiance) ?
				glm::max(sample.m_radiance, glm::vec3(0.0f)) : glm::vec3(0.0f);
			const float projectionScale = 1.0f /
				(static_cast<float>(rayCount) * directionPdf);
			float basis[GIProbeSphericalHarmonicsCoefficientCount]{};
			EvaluateSphericalHarmonicsBasis(direction, basis);
			for (uint32_t coefficientIndex = 0u;
				coefficientIndex < GIProbeSphericalHarmonicsCoefficientCount;
				++coefficientIndex)
			{
				probe.m_irradiance[coefficientIndex] += radiance *
					basis[coefficientIndex] * projectionScale *
					IrradianceConvolution(coefficientIndex);
			}
		}
		return true;
	}

	bool ValidateRequest(
		const GIProbesBakeRequest& request,
		std::string& outDiagnostic)
	{
		outDiagnostic.clear();
		if (request.m_stateName.empty())
		{
			outDiagnostic = "a .probes bake requires a non-empty state name";
			return false;
		}
		if (request.m_settings.m_raysPerProbe == 0u ||
			request.m_settings.m_bounceCount == 0u)
		{
			outDiagnostic =
				"a .probes bake requires non-zero ray and bounce counts";
			return false;
		}
		if (request.m_threadCount == 0u ||
			request.m_threadCount > GIProbesMaxBakeThreadCount)
		{
			outDiagnostic =
				"a .probes bake requires a supported non-zero thread count";
			return false;
		}
		if (request.m_settings.m_raysPerProbe >
				GIProbesMaxRaysPerProbe ||
			request.m_settings.m_bounceCount >
				GIProbesMaxBounceCount ||
			request.m_settings.m_maxSubdivisionLevel >
				GIProbesMaxSubdivisionLevel)
		{
			outDiagnostic =
				"a .probes bake exceeds the supported sampling limits";
			return false;
		}
		if (!std::isfinite(request.m_settings.m_skyIndirectIntensity) ||
			request.m_settings.m_skyIndirectIntensity < 0.0f)
		{
			outDiagnostic =
				"a .probes bake requires a finite non-negative sky GI indirect intensity";
			return false;
		}
		if (request.m_layoutSource)
		{
			return true;
		}
		const glm::vec3 volumeExtent =
			request.m_volumeMax - request.m_volumeMin;
		if (!IsFinite(request.m_volumeMin) ||
			!IsFinite(request.m_volumeMax) ||
			!IsFinite(volumeExtent) ||
			glm::any(glm::lessThanEqual(
				request.m_volumeMax,
				request.m_volumeMin)))
		{
			outDiagnostic = "a .probes bake requires finite non-empty volume bounds";
			return false;
		}
		if (!std::isfinite(request.m_settings.m_minProbeSpacing) ||
			request.m_settings.m_minProbeSpacing <= 0.0f ||
			!std::isfinite(request.m_settings.m_normalBias) ||
			request.m_settings.m_normalBias < 0.0f ||
			!std::isfinite(request.m_settings.m_viewBias) ||
			request.m_settings.m_viewBias < 0.0f ||
			!std::isfinite(request.m_settings.m_maxRayDistance) ||
			request.m_settings.m_maxRayDistance <= 0.0f)
		{
			outDiagnostic = "a .probes bake requires valid sampling settings";
			return false;
		}
		if (IntersectsGeometryNeighborhood(
				request.m_volumeMin,
				request.m_volumeMax,
				request.m_settings.m_minProbeSpacing,
				request.m_sceneGeometryBounds))
		{
			const uint32_t requiredSubdivisionLevel =
				CalculateRequiredSubdivisionLevel(
					volumeExtent,
					request.m_settings.m_minProbeSpacing);
			if (requiredSubdivisionLevel >
				request.m_settings.m_maxSubdivisionLevel)
			{
				outDiagnostic =
					"max subdivision level " +
					std::to_string(
						request.m_settings.m_maxSubdivisionLevel) +
					" cannot honor min probe spacing " +
					std::to_string(
						request.m_settings.m_minProbeSpacing) +
					" for these volume bounds; use at least level " +
					std::to_string(requiredSubdivisionLevel);
				return false;
			}
		}
		return true;
	}
}

bool IGIProbeBakeRaySampler::SamplePrimaryDirection(
	const glm::vec3& uniformDirection,
	uint32_t,
	uint32_t,
	uint32_t,
	glm::vec3& outDirection,
	float& outPdf,
	std::string& outDiagnostic) const
{
	outDirection = uniformDirection;
	outPdf = 1.0f / (4.0f * Pi);
	outDiagnostic.clear();
	return true;
}

GIProbesBakeResult GIProbesBaker::Bake(
	const GIProbesBakeRequest& request,
	const IGIProbeBakeRaySampler& sampler) noexcept
{
	GIProbesBakeResult result;
	try
	{
		if (!ValidateRequest(request, result.m_diagnostic))
		{
			result.m_status = EGIProbesBakeStatus::InvalidRequest;
			return result;
		}
		if (IsCancelled(request))
		{
			result.m_status = EGIProbesBakeStatus::Cancelled;
			result.m_diagnostic = "GI probe bake was cancelled before layout generation";
			return result;
		}

		Utils::Timer timer;
		timer.Start();
		GIProbesDataPtr data = GIProbesDataPtr::Make();
		data->m_bakeSettings = request.m_settings;
		data->m_stateName = request.m_stateName;
		data->m_bakerVersion = request.m_bakerVersion;
		data->m_sourceWorldHash = request.m_sourceWorldHash;
		data->m_representationHash = ComputeGIProbesRepresentationHash(
			data->m_formatVersion,
			data->m_shOrder,
			data->m_compression);
		if (!BuildAdaptiveLayout(request, *data, result.m_diagnostic))
		{
			result.m_status = EGIProbesBakeStatus::InvalidRequest;
			return result;
		}

		const bool bReuseTransport = request.m_layoutSource != nullptr;
		GIProbesBakeRequest effectiveRequest = request;
		effectiveRequest.m_settings = data->m_bakeSettings;
		effectiveRequest.m_volumeMin = data->m_volumeMin;
		effectiveRequest.m_volumeMax = data->m_volumeMax;
		const uint32_t totalProbes = static_cast<uint32_t>(
			data->m_probes.Num());
		const std::vector<float> visibilityMaxDistances =
			CalculateProbeVisibilityMaxDistances(*data);
		const uint32_t threadCount = (std::min)(
			request.m_threadCount,
			totalProbes);
		GIProbesBakeProgress progress;
		progress.m_totalProbes = totalProbes;
		progress.m_stage = (bReuseTransport ?
			"Baking lighting with reused layout" :
			"Baking adaptive GI probes") +
			std::string(" (") + std::to_string(threadCount) +
			(threadCount == 1u ? " thread)" : " threads)");
		if (request.m_progress)
		{
			request.m_progress(progress);
		}

		std::atomic<uint32_t> nextProbeIndex{ 0u };
		std::atomic<bool> stopWorkers{ false };
		std::atomic<EGIProbesBakeStatus> workerStatus{
			EGIProbesBakeStatus::Success };
		std::mutex failureMutex;
		std::string failureDiagnostic;
		std::mutex progressMutex;
		uint32_t completedProbes = 0u;

		const auto recordFailure = [&workerStatus,
			&stopWorkers,
			&failureMutex,
			&failureDiagnostic](
				EGIProbesBakeStatus status,
				std::string diagnostic)
		{
			EGIProbesBakeStatus expected = EGIProbesBakeStatus::Success;
			if (workerStatus.compare_exchange_strong(
					expected,
					status,
					std::memory_order_acq_rel))
			{
				const std::lock_guard<std::mutex> lock(failureMutex);
				failureDiagnostic = std::move(diagnostic);
			}
			stopWorkers.store(true, std::memory_order_release);
		};

		const auto bakeWorker = [&]()
		{
			try
			{
				while (!stopWorkers.load(std::memory_order_acquire))
				{
					if (IsCancelled(effectiveRequest))
					{
						stopWorkers.store(true, std::memory_order_release);
						return;
					}
					const uint32_t probeIndex = nextProbeIndex.fetch_add(
						1u,
						std::memory_order_relaxed);
					if (probeIndex >= totalProbes)
					{
						return;
					}

					std::string diagnostic;
					if (!BakeProbe(
							effectiveRequest,
							sampler,
							probeIndex,
							visibilityMaxDistances[probeIndex],
							bReuseTransport,
							data->m_probes[probeIndex],
							diagnostic))
					{
						if (IsCancelled(effectiveRequest))
						{
							stopWorkers.store(true, std::memory_order_release);
						}
						else
						{
							recordFailure(
								EGIProbesBakeStatus::SamplingFailed,
								std::move(diagnostic));
						}
						return;
					}

					if (stopWorkers.load(std::memory_order_acquire))
					{
						return;
					}
					const std::lock_guard<std::mutex> lock(progressMutex);
					++completedProbes;
					if (request.m_progress)
					{
						GIProbesBakeProgress workerProgress = progress;
						workerProgress.m_completedProbes = completedProbes;
						workerProgress.m_fraction = static_cast<float>(
							completedProbes) / static_cast<float>(totalProbes);
						request.m_progress(workerProgress);
					}
				}
			}
			catch (const std::exception& exception)
			{
				recordFailure(
					EGIProbesBakeStatus::InvalidResult,
					std::string("GI probe bake worker failed: ") +
						exception.what());
			}
			catch (...)
			{
				recordFailure(
					EGIProbesBakeStatus::InvalidResult,
					"GI probe bake worker failed with an unknown error");
			}
		};

		std::vector<std::thread> workers;
		workers.reserve(threadCount > 0u ? threadCount - 1u : 0u);
		try
		{
			for (uint32_t threadIndex = 1u;
				threadIndex < threadCount;
				++threadIndex)
			{
				workers.emplace_back(bakeWorker);
			}
		}
		catch (...)
		{
			stopWorkers.store(true, std::memory_order_release);
			for (std::thread& worker : workers)
			{
				worker.join();
			}
			throw;
		}
		bakeWorker();
		for (std::thread& worker : workers)
		{
			worker.join();
		}

		if (IsCancelled(effectiveRequest))
		{
			result.m_status = EGIProbesBakeStatus::Cancelled;
			result.m_diagnostic = "GI probe bake was cancelled";
			return result;
		}
		result.m_status = workerStatus.load(std::memory_order_acquire);
		if (result.m_status != EGIProbesBakeStatus::Success)
		{
			const std::lock_guard<std::mutex> lock(failureMutex);
			result.m_diagnostic = failureDiagnostic;
			return result;
		}

		CanonicalizeSharedProbeSamples(*data, !bReuseTransport);
		if (!bReuseTransport)
		{
			data->m_layoutHash = ComputeGIProbesLayoutHash(*data);
			data->m_transportHash = ComputeTransportHash(*data);
		}
		data->m_lightingHash = ComputeLightingHash(*data);
		float validity = 0.0f;
		for (const GIProbe& probe : data->m_probes)
		{
			validity += probe.m_validity;
			if ((probe.m_flags & static_cast<uint32_t>(
				EGIProbeFlag::Valid)) == 0u)
			{
				++data->m_diagnostics.m_invalidProbeCount;
			}
			if ((probe.m_flags & static_cast<uint32_t>(
				EGIProbeFlag::Relocated)) != 0u)
			{
				++data->m_diagnostics.m_relocatedProbeCount;
			}
		}
		data->m_diagnostics.m_averageValidity = validity /
			static_cast<float>(data->m_probes.Num());
		timer.Stop();
		data->m_diagnostics.m_bakeDurationSeconds =
			timer.ResultAccumulatedMs() / 1000.0f;
		data->m_diagnostics.m_message = bReuseTransport ?
			"baked one lighting state using compatible layout and transport" :
			"baked one adaptive irradiance-probe state";

		std::string validationDiagnostic;
		if (!data->Validate(validationDiagnostic))
		{
			result.m_status = EGIProbesBakeStatus::InvalidResult;
			result.m_diagnostic = "baker produced invalid .probes data: " +
				validationDiagnostic;
			return result;
		}

		result.m_status = EGIProbesBakeStatus::Success;
		result.m_data = std::move(data);
		result.m_diagnostic = result.m_data->m_diagnostics.m_message;
		return result;
	}
	catch (const std::exception& exception)
	{
		result.m_status = EGIProbesBakeStatus::InvalidResult;
		result.m_data.Clear();
		result.m_diagnostic = std::string("GI probe bake failed: ") +
			exception.what();
		return result;
	}
	catch (...)
	{
		result.m_status = EGIProbesBakeStatus::InvalidResult;
		result.m_data.Clear();
		result.m_diagnostic = "GI probe bake failed with an unknown error";
		return result;
	}
}
