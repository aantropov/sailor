#include "AssetRegistry/GlobalIllumination/ProbeVolumeBaker.h"

#include "Containers/Hash.h"
#include "Core/Utils.h"

#include <algorithm>
#include <array>
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

	const std::array<glm::vec3, ProbeVolumeVisibilityDirectionCount>
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

	bool IsCancelled(const ProbeVolumeBakeRequest& request) noexcept
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

	uint64_t ComputeTransportHash(const ProbeVolumeData& data) noexcept
	{
		uint64_t hash = 1469598103934665603ull;
		HashValue(hash, data.m_layoutHash);
		HashValue(hash, data.m_bakeSettings.m_maxSubdivisionLevel);
		HashValue(hash, data.m_bakeSettings.m_minProbeSpacing);
		HashValue(hash, data.m_bakeSettings.m_normalBias);
		HashValue(hash, data.m_bakeSettings.m_viewBias);
		HashValue(hash, data.m_bakeSettings.m_maxRayDistance);
		for (const ProbeVolumeSample& probe : data.m_probes)
		{
			HashVec3(hash, probe.m_relocationOffset);
			HashValue(hash, probe.m_validity);
			HashValue(hash, probe.m_flags);
			for (const glm::vec2& moments : probe.m_visibility)
			{
				HashVec2(hash, moments);
			}
		}
		return hash;
	}

	uint64_t ComputeLightingHash(const ProbeVolumeData& data) noexcept
	{
		uint64_t hash = 1469598103934665603ull;
		HashValue(hash, data.m_layoutHash);
		HashValue(hash, data.m_bakeSettings.m_raysPerProbe);
		HashValue(hash, data.m_bakeSettings.m_bounceCount);
		HashValue(hash, data.m_bakeSettings.m_randomSeed);
		for (const ProbeVolumeSample& probe : data.m_probes)
		{
			for (const glm::vec3& coefficient : probe.m_irradiance)
			{
				HashVec3(hash, coefficient);
			}
		}
		return hash;
	}

	bool Intersects(
		const glm::vec3& min,
		const glm::vec3& max,
		const Math::AABB& bounds) noexcept
	{
		return bounds.IsValid() &&
			glm::all(glm::lessThanEqual(min, bounds.m_max)) &&
			glm::all(glm::greaterThanEqual(max, bounds.m_min));
	}

	bool IntersectsGeometry(
		const glm::vec3& min,
		const glm::vec3& max,
		const TVector<Math::AABB>& geometryBounds) noexcept
	{
		for (const Math::AABB& bounds : geometryBounds)
		{
			if (Intersects(min, max, bounds))
			{
				return true;
			}
		}
		return false;
	}

	bool AppendBrick(
		ProbeVolumeData& data,
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

		ProbeVolumeBrick brick;
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
					ProbeVolumeSample probe;
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
		const ProbeVolumeBakeRequest& request,
		ProbeVolumeData& data,
		std::string& outDiagnostic)
	{
		if (request.m_layoutSource)
		{
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
			for (ProbeVolumeSample& probe : data.m_probes)
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
			const bool bCanSubdivide =
				subdivisionLevel < request.m_settings.m_maxSubdivisionLevel &&
				glm::all(glm::greaterThanEqual(
					childExtent,
					glm::vec3(request.m_settings.m_minProbeSpacing)));
			const bool bShouldSubdivide = bCanSubdivide &&
				IntersectsGeometry(min, max, request.m_sceneGeometryBounds);
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
			for (uint32_t z = 0u; z < 2u; ++z)
			{
				for (uint32_t y = 0u; y < 2u; ++y)
				{
					for (uint32_t x = 0u; x < 2u; ++x)
					{
						const glm::bvec3 upper(x != 0u, y != 0u, z != 0u);
						const glm::vec3 childMin(
							upper.x ? center.x : min.x,
							upper.y ? center.y : min.y,
							upper.z ? center.z : min.z);
						const glm::vec3 childMax(
							upper.x ? max.x : center.x,
							upper.y ? max.y : center.y,
							upper.z ? max.z : center.z);
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
		data.m_layoutHash = ComputeProbeVolumeLayoutHash(data);
		return true;
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
		const IProbeVolumeBakeRaySampler& sampler,
		const glm::vec3& position,
		float maxDistance,
		uint32_t baseSeed,
		uint32_t probeIndex,
		uint32_t stream,
		std::array<float, ProbeVolumeVisibilityDirectionCount>& outDistances,
		std::string& outDiagnostic)
	{
		for (uint32_t directionIndex = 0u;
			directionIndex < ProbeVolumeVisibilityDirectionCount;
			++directionIndex)
		{
			ProbeVolumeBakeRaySample sample;
			if (!sampler.Sample(
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
		const std::array<float, ProbeVolumeVisibilityDirectionCount>& distances,
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

	bool BakeProbe(
		const ProbeVolumeBakeRequest& request,
		const IProbeVolumeBakeRaySampler& sampler,
		uint32_t probeIndex,
		bool bReuseTransport,
		ProbeVolumeSample& probe,
		std::string& outDiagnostic)
	{
		std::array<float, ProbeVolumeVisibilityDirectionCount>
			clearanceDistances{};
		if (!bReuseTransport)
		{
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
				const glm::vec3 originalPosition = probe.m_position;
				probe.m_position = glm::clamp(
					originalPosition + probe.m_relocationOffset,
					request.m_volumeMin,
					request.m_volumeMax);
				probe.m_relocationOffset = probe.m_position - originalPosition;
				if (glm::length(probe.m_relocationOffset) > 1e-6f)
				{
					probe.m_flags |= static_cast<uint32_t>(
						EProbeVolumeSampleFlag::Relocated);
					if (!SampleVisibility(
							sampler,
							probe.m_position,
							request.m_settings.m_maxRayDistance,
							request.m_settings.m_randomSeed,
							probeIndex,
							1u,
							clearanceDistances,
							outDiagnostic))
					{
						return false;
					}
				}
			}

			const float targetClearance = (std::max)(
				request.m_settings.m_minProbeSpacing * 0.25f,
				0.001f);
			float averageClearance = 0.0f;
			for (uint32_t directionIndex = 0u;
				directionIndex < ProbeVolumeVisibilityDirectionCount;
				++directionIndex)
			{
				const float distance = clearanceDistances[directionIndex];
				averageClearance += distance;
			}
			averageClearance /=
				static_cast<float>(ProbeVolumeVisibilityDirectionCount);
			probe.m_validity = glm::clamp(
				averageClearance / targetClearance,
				0.0f,
				1.0f);
			if (probe.m_validity <= 0.05f)
			{
				probe.m_flags &= ~static_cast<uint32_t>(
					EProbeVolumeSampleFlag::Valid);
			}
			else
			{
				probe.m_flags |= static_cast<uint32_t>(
					EProbeVolumeSampleFlag::Valid);
			}
		}

		probe.m_irradiance = {};
		std::array<float, ProbeVolumeVisibilityDirectionCount>
			visibilityDistanceSums{};
		std::array<float, ProbeVolumeVisibilityDirectionCount>
			visibilityDistanceSquaredSums{};
		std::array<uint32_t, ProbeVolumeVisibilityDirectionCount>
			visibilitySampleCounts{};
		const uint32_t rayCount = request.m_settings.m_raysPerProbe;
		const float projectionScale = 4.0f * Pi /
			static_cast<float>(rayCount);
		for (uint32_t rayIndex = 0u; rayIndex < rayCount; ++rayIndex)
		{
			if (IsCancelled(request))
			{
				outDiagnostic = "probe-volume bake was cancelled";
				return false;
			}
			const glm::vec3 direction = FibonacciDirection(
				rayIndex,
				rayCount,
				request.m_settings.m_randomSeed,
				probeIndex);
			ProbeVolumeBakeRaySample sample;
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
			if (!bReuseTransport)
			{
				const glm::vec3 absoluteDirection = glm::abs(direction);
				const uint32_t axis = absoluteDirection.x >= absoluteDirection.y &&
					absoluteDirection.x >= absoluteDirection.z ? 0u :
					absoluteDirection.y >= absoluteDirection.z ? 1u : 2u;
				const uint32_t directionIndex = axis * 2u +
					(direction[axis] >= 0.0f ? 0u : 1u);
				const float distance = sample.m_bHit ?
					glm::clamp(
						sample.m_distance,
						0.0f,
						request.m_settings.m_maxRayDistance) :
					request.m_settings.m_maxRayDistance;
				visibilityDistanceSums[directionIndex] += distance;
				visibilityDistanceSquaredSums[directionIndex] +=
					distance * distance;
				++visibilitySampleCounts[directionIndex];
			}
			float basis[ProbeVolumeSphericalHarmonicsCoefficientCount]{};
			EvaluateSphericalHarmonicsBasis(direction, basis);
			for (uint32_t coefficientIndex = 0u;
				coefficientIndex < ProbeVolumeSphericalHarmonicsCoefficientCount;
				++coefficientIndex)
			{
				probe.m_irradiance[coefficientIndex] += radiance *
					basis[coefficientIndex] * projectionScale *
					IrradianceConvolution(coefficientIndex);
			}
		}
		if (!bReuseTransport)
		{
			for (uint32_t directionIndex = 0u;
				directionIndex < ProbeVolumeVisibilityDirectionCount;
				++directionIndex)
			{
				const uint32_t sampleCount =
					visibilitySampleCounts[directionIndex];
				if (sampleCount > 0u)
				{
					const float inverseSampleCount =
						1.0f / static_cast<float>(sampleCount);
					probe.m_visibility[directionIndex] = glm::vec2(
						visibilityDistanceSums[directionIndex] *
							inverseSampleCount,
						visibilityDistanceSquaredSums[directionIndex] *
							inverseSampleCount);
				}
				else
				{
					const float distance =
						clearanceDistances[directionIndex];
					probe.m_visibility[directionIndex] =
						glm::vec2(distance, distance * distance);
				}
			}
		}
		return true;
	}

	bool ValidateRequest(
		const ProbeVolumeBakeRequest& request,
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
			request.m_threadCount > ProbeVolumeMaxBakeThreadCount)
		{
			outDiagnostic =
				"a .probes bake requires a supported non-zero thread count";
			return false;
		}
		if (request.m_settings.m_raysPerProbe >
				ProbeVolumeMaxRaysPerProbe ||
			request.m_settings.m_bounceCount >
				ProbeVolumeMaxBounceCount ||
			request.m_settings.m_maxSubdivisionLevel >
				ProbeVolumeMaxSubdivisionLevel)
		{
			outDiagnostic =
				"a .probes bake exceeds the supported sampling limits";
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
		return true;
	}
}

ProbeVolumeBakeResult ProbeVolumeBaker::Bake(
	const ProbeVolumeBakeRequest& request,
	const IProbeVolumeBakeRaySampler& sampler) noexcept
{
	ProbeVolumeBakeResult result;
	try
	{
		if (!ValidateRequest(request, result.m_diagnostic))
		{
			result.m_status = EProbeVolumeBakeStatus::InvalidRequest;
			return result;
		}
		if (IsCancelled(request))
		{
			result.m_status = EProbeVolumeBakeStatus::Cancelled;
			result.m_diagnostic = "probe-volume bake was cancelled before layout generation";
			return result;
		}

		Utils::Timer timer;
		timer.Start();
		ProbeVolumeDataPtr data = ProbeVolumeDataPtr::Make();
		data->m_bakeSettings = request.m_settings;
		data->m_stateName = request.m_stateName;
		data->m_bakerVersion = request.m_bakerVersion;
		data->m_sourceWorldHash = request.m_sourceWorldHash;
		data->m_representationHash = ComputeProbeVolumeRepresentationHash(
			data->m_formatVersion,
			data->m_shOrder,
			data->m_compression);
		if (!BuildAdaptiveLayout(request, *data, result.m_diagnostic))
		{
			result.m_status = EProbeVolumeBakeStatus::InvalidRequest;
			return result;
		}

		const bool bReuseTransport = request.m_layoutSource != nullptr;
		ProbeVolumeBakeRequest effectiveRequest = request;
		effectiveRequest.m_settings = data->m_bakeSettings;
		effectiveRequest.m_volumeMin = data->m_volumeMin;
		effectiveRequest.m_volumeMax = data->m_volumeMax;
		const uint32_t totalProbes = static_cast<uint32_t>(
			data->m_probes.Num());
		const uint32_t threadCount = (std::min)(
			request.m_threadCount,
			totalProbes);
		ProbeVolumeBakeProgress progress;
		progress.m_totalProbes = totalProbes;
		progress.m_stage = (bReuseTransport ?
			"Baking lighting with reused layout" :
			"Baking adaptive probe volume") +
			std::string(" (") + std::to_string(threadCount) +
			(threadCount == 1u ? " thread)" : " threads)");
		if (request.m_progress)
		{
			request.m_progress(progress);
		}

		std::atomic<uint32_t> nextProbeIndex{ 0u };
		std::atomic<bool> stopWorkers{ false };
		std::atomic<EProbeVolumeBakeStatus> workerStatus{
			EProbeVolumeBakeStatus::Success };
		std::mutex failureMutex;
		std::string failureDiagnostic;
		std::mutex progressMutex;
		uint32_t completedProbes = 0u;

		const auto recordFailure = [&workerStatus,
			&stopWorkers,
			&failureMutex,
			&failureDiagnostic](
				EProbeVolumeBakeStatus status,
				std::string diagnostic)
		{
			EProbeVolumeBakeStatus expected = EProbeVolumeBakeStatus::Success;
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
								EProbeVolumeBakeStatus::SamplingFailed,
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
						ProbeVolumeBakeProgress workerProgress = progress;
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
					EProbeVolumeBakeStatus::InvalidResult,
					std::string("probe-volume bake worker failed: ") +
						exception.what());
			}
			catch (...)
			{
				recordFailure(
					EProbeVolumeBakeStatus::InvalidResult,
					"probe-volume bake worker failed with an unknown error");
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
			result.m_status = EProbeVolumeBakeStatus::Cancelled;
			result.m_diagnostic = "probe-volume bake was cancelled";
			return result;
		}
		result.m_status = workerStatus.load(std::memory_order_acquire);
		if (result.m_status != EProbeVolumeBakeStatus::Success)
		{
			const std::lock_guard<std::mutex> lock(failureMutex);
			result.m_diagnostic = failureDiagnostic;
			return result;
		}

		if (!bReuseTransport)
		{
			data->m_layoutHash = ComputeProbeVolumeLayoutHash(*data);
			data->m_transportHash = ComputeTransportHash(*data);
		}
		data->m_lightingHash = ComputeLightingHash(*data);
		float validity = 0.0f;
		for (const ProbeVolumeSample& probe : data->m_probes)
		{
			validity += probe.m_validity;
			if ((probe.m_flags & static_cast<uint32_t>(
				EProbeVolumeSampleFlag::Valid)) == 0u)
			{
				++data->m_diagnostics.m_invalidProbeCount;
			}
			if ((probe.m_flags & static_cast<uint32_t>(
				EProbeVolumeSampleFlag::Relocated)) != 0u)
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
			result.m_status = EProbeVolumeBakeStatus::InvalidResult;
			result.m_diagnostic = "baker produced invalid .probes data: " +
				validationDiagnostic;
			return result;
		}

		result.m_status = EProbeVolumeBakeStatus::Success;
		result.m_data = std::move(data);
		result.m_diagnostic = result.m_data->m_diagnostics.m_message;
		return result;
	}
	catch (const std::exception& exception)
	{
		result.m_status = EProbeVolumeBakeStatus::InvalidResult;
		result.m_data.Clear();
		result.m_diagnostic = std::string("probe-volume bake failed: ") +
			exception.what();
		return result;
	}
	catch (...)
	{
		result.m_status = EProbeVolumeBakeStatus::InvalidResult;
		result.m_data.Clear();
		result.m_diagnostic = "probe-volume bake failed with an unknown error";
		return result;
	}
}
