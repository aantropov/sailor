#include "GlobalIllumination/GIProbesTracing.h"
#include "Math/Math.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Sailor;

namespace
{
	constexpr float Pi = 3.14159265358979323846f;
	constexpr uint32_t ProbeTransportRayCount = 64u;
	constexpr uint32_t MaxProbeRelocationIterations = 4u;
	constexpr float MaxValidBackfaceRatio = 0.25f;

	uint32_t MixGIProbeRandomSeed(
		uint32_t baseSeed,
		uint32_t probeSeed,
		uint32_t sampleIndex,
		uint32_t stream) noexcept
	{
		uint32_t value = baseSeed ^ 0x9e3779b9u;
		value ^= probeSeed * 0x85ebca6bu + 0xc2b2ae35u;
		value ^= sampleIndex * 0x27d4eb2du + stream * 0x165667b1u;
		value ^= value >> 16u;
		value *= 0x7feb352du;
		value ^= value >> 15u;
		value *= 0x846ca68bu;
		value ^= value >> 16u;
		return value != 0u ? value : 0x6d2b79f5u;
	}

	glm::vec3 GenerateGIProbeFibonacciDirection(
		uint32_t index,
		uint32_t count,
		uint32_t seed,
		uint32_t probeSeed) noexcept
	{
		if (count == 0u)
		{
			return glm::vec3(0.0f, 1.0f, 0.0f);
		}
		const float offset = 2.0f / static_cast<float>(count);
		const float y = static_cast<float>(index) * offset - 1.0f +
			offset * 0.5f;
		const float radius = std::sqrt((std::max)(0.0f, 1.0f - y * y));
		const uint32_t rotationBits =
			seed * 747796405u + probeSeed * 2891336453u;
		const float rotation = static_cast<float>(rotationBits & 0x00ffffffu) /
			16777216.0f * 2.0f * Pi;
		const float phi = static_cast<float>(index) * 2.39996322972865332f +
			rotation;
		return glm::vec3(
			std::cos(phi) * radius,
			y,
			std::sin(phi) * radius);
	}

	bool IsCancelled(
		const GIProbeTraceRequest& request) noexcept
	{
		return request.m_cancel &&
			request.m_cancel->load(std::memory_order_acquire);
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
		return coefficientIndex == 0u ? 1.0f :
			coefficientIndex <= 3u ? 2.0f / 3.0f : 0.25f;
	}

	bool SampleVisibility(
		const GIProbeTraceRequest& request,
		const IGIProbeBakeRaySampler& sampler,
		const glm::vec3& position,
		uint32_t probeSeed,
		uint32_t stream,
		std::array<float, GIProbeVisibilityDirectionCount>& outDistances,
		std::string& outDiagnostic)
	{
		for (uint32_t directionIndex = 0u;
			directionIndex < GIProbeVisibilityDirectionCount;
			++directionIndex)
		{
			if (IsCancelled(request))
			{
				outDiagnostic = "GI probe tracing was cancelled";
				return false;
			}
			GIProbeBakeRaySample sample;
			if (!sampler.SampleVisibility(
					position,
					GIProbeVisibilityDirections[directionIndex],
					request.m_settings.m_maxRayDistance,
					MixGIProbeRandomSeed(
						request.m_settings.m_randomSeed,
						probeSeed,
						directionIndex,
						stream),
					sample,
					outDiagnostic))
			{
				return false;
			}
			outDistances[directionIndex] = sample.m_bHit ?
				glm::clamp(
					sample.m_distance,
					0.0f,
					request.m_settings.m_maxRayDistance) :
				request.m_settings.m_maxRayDistance;
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
		std::array<double, GIProbeVisibilityDirectionCount> m_weights{};
		std::array<double, GIProbeVisibilityDirectionCount>
			m_environmentVisibilitySums{};
		std::array<double, GIProbeVisibilityDirectionCount> m_distanceSums{};
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
		const GIProbeTraceRequest& request,
		const IGIProbeBakeRaySampler& sampler,
		const glm::vec3& position,
		float targetClearance,
		float visibilityMaxDistance,
		uint32_t probeSeed,
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
				outDiagnostic = "GI probe tracing was cancelled";
				return false;
			}

			// Fixed transport directions prevent random visibility islands from
			// moving between neighbouring probes and duplicate brick corners.
			const glm::vec3 direction = GenerateGIProbeFibonacciDirection(
				rayIndex,
				ProbeTransportRayCount,
				0u,
				0u);
			GIProbeBakeRaySample sample;
			if (!sampler.SampleVisibility(
					position,
					direction,
					request.m_settings.m_maxRayDistance,
					MixGIProbeRandomSeed(
						request.m_settings.m_randomSeed,
						probeSeed,
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
					glm::clamp(
						sample.m_distance,
						0.0f,
						visibilityMaxDistance) :
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
					outTransport.m_closestBackfaceDistance = sample.m_distance;
					outTransport.m_closestBackfaceDirection = direction;
				}
			}
			else if (sample.m_bHit &&
				std::isfinite(sample.m_distance) &&
				sample.m_distance < targetClearance)
			{
				const float clearanceDeficit = targetClearance -
					(std::max)(sample.m_distance, 0.0f);
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
				probe.m_flags |= GIProbeBlockedDirectionBit(directionIndex);
			}
			probe.m_environmentVisibility[directionIndex] = weight > 0.0 ?
				static_cast<float>(
					transport.m_environmentVisibilitySums[directionIndex] /
						weight) :
				1.0f;
		}
	}

}

bool Sailor::TraceGIProbeTransport(
	const GIProbeTraceRequest& request,
	const IGIProbeBakeRaySampler& sampler,
	uint32_t probeSeed,
	float visibilityMaxDistance,
	GIProbe& probe,
	std::string& outDiagnostic)
{
	outDiagnostic.clear();
	if (!Math::AllFinite(request.m_volumeMin) ||
		!Math::AllFinite(request.m_volumeMax) ||
		glm::any(glm::lessThanEqual(
			request.m_volumeMax,
			request.m_volumeMin)) ||
		!Math::AllFinite(probe.m_position) ||
		!std::isfinite(request.m_settings.m_minProbeSpacing) ||
		request.m_settings.m_minProbeSpacing <= 0.0f ||
		!std::isfinite(request.m_settings.m_maxRayDistance) ||
		request.m_settings.m_maxRayDistance <= 0.0f ||
		!std::isfinite(visibilityMaxDistance) ||
		visibilityMaxDistance <= 0.0f)
	{
		outDiagnostic = "GI probe transport request contains invalid geometry";
		return false;
	}
	if (IsCancelled(request))
	{
		outDiagnostic = "GI probe tracing was cancelled";
		return false;
	}

	GIProbe workingProbe = probe;
	const glm::vec3 originalPosition = workingProbe.m_position;
	std::array<float, GIProbeVisibilityDirectionCount> clearanceDistances{};
	if (!SampleVisibility(
			request,
			sampler,
			workingProbe.m_position,
			probeSeed,
			0u,
			clearanceDistances,
			outDiagnostic))
	{
		return false;
	}

	workingProbe.m_relocationOffset = CalculateRelocation(
		clearanceDistances,
		request.m_settings.m_minProbeSpacing);
	if (glm::length(workingProbe.m_relocationOffset) > 1e-6f)
	{
		workingProbe.m_position = glm::clamp(
			originalPosition + workingProbe.m_relocationOffset,
			request.m_volumeMin,
			request.m_volumeMax);
		workingProbe.m_relocationOffset =
			workingProbe.m_position - originalPosition;
		if (glm::length(workingProbe.m_relocationOffset) > 1e-6f)
		{
			workingProbe.m_flags |= static_cast<uint32_t>(
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
			workingProbe.m_position,
			targetClearance,
			visibilityMaxDistance,
			probeSeed,
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
			if (!std::isfinite(transport.m_closestBackfaceDistance) ||
				transport.m_closestBackfaceDistance >=
					(std::numeric_limits<float>::max)() ||
				glm::length(transport.m_closestBackfaceDirection) <= 1e-6f)
			{
				break;
			}
			relocationStep = transport.m_closestBackfaceDirection *
				(transport.m_closestBackfaceDistance + targetClearance);
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
			workingProbe.m_position - originalPosition + relocationStep;
		const float requestedLength = glm::length(requestedOffset);
		if (requestedLength > maxRelocation && requestedLength > 1e-6f)
		{
			requestedOffset *= maxRelocation / requestedLength;
		}
		const glm::vec3 relocatedPosition = glm::clamp(
			originalPosition + requestedOffset,
			request.m_volumeMin,
			request.m_volumeMax);
		if (glm::length(relocatedPosition - workingProbe.m_position) <= 1e-6f)
		{
			break;
		}
		workingProbe.m_position = relocatedPosition;
		workingProbe.m_relocationOffset =
			workingProbe.m_position - originalPosition;
		workingProbe.m_flags |= static_cast<uint32_t>(
			EGIProbeFlag::Relocated);
		if (!SampleProbeTransport(
				request,
				sampler,
				workingProbe.m_position,
				targetClearance,
				visibilityMaxDistance,
				probeSeed,
				2u + iteration,
				transport,
				outDiagnostic))
		{
			return false;
		}
	}

	workingProbe.m_validity = IsEmbeddedProbe(transport) ? 0.0f : 1.0f;
	if (workingProbe.m_validity > 0.05f)
	{
		workingProbe.m_flags |= static_cast<uint32_t>(EGIProbeFlag::Valid);
	}
	else
	{
		workingProbe.m_flags &=
			~static_cast<uint32_t>(EGIProbeFlag::Valid);
	}
	StoreProbeVisibility(
		transport,
		visibilityMaxDistance,
		workingProbe);
	probe = workingProbe;
	return true;
}

bool Sailor::AccumulateGIProbeIrradianceRange(
	const GIProbeTraceRequest& request,
	const IGIProbeBakeRaySampler& sampler,
	const glm::vec3& position,
	uint32_t probeSeed,
	uint32_t sampleBegin,
	uint32_t sampleCount,
	uint32_t sequenceSampleCount,
	GIProbeIrradianceAccumulator& accumulator,
	std::string& outDiagnostic)
{
	outDiagnostic.clear();
	if (sequenceSampleCount == 0u ||
		sequenceSampleCount > GIProbesMaxRaysPerProbe ||
		sampleCount == 0u ||
		sampleBegin > sequenceSampleCount ||
		sampleCount > sequenceSampleCount - sampleBegin)
	{
		outDiagnostic = "GI probe irradiance range is outside its sample sequence";
		return false;
	}
	if (accumulator.m_sampleCount != sampleBegin ||
		(accumulator.m_sequenceSampleCount != 0u &&
			accumulator.m_sequenceSampleCount != sequenceSampleCount))
	{
		outDiagnostic =
			"GI probe irradiance ranges must be accumulated once in sequence order";
		return false;
	}
	if (!Math::AllFinite(position) ||
		!std::isfinite(request.m_settings.m_maxRayDistance) ||
		request.m_settings.m_maxRayDistance <= 0.0f)
	{
		outDiagnostic = "GI probe irradiance request contains invalid geometry";
		return false;
	}
	if (IsCancelled(request))
	{
		outDiagnostic = "GI probe tracing was cancelled";
		return false;
	}

	GIProbeIrradianceAccumulator workingAccumulator = accumulator;
	workingAccumulator.m_sequenceSampleCount = sequenceSampleCount;
	const uint32_t sampleEnd = sampleBegin + sampleCount;
	for (uint32_t rayIndex = sampleBegin; rayIndex < sampleEnd; ++rayIndex)
	{
		if (IsCancelled(request))
		{
			outDiagnostic = "GI probe tracing was cancelled";
			return false;
		}
		const glm::vec3 uniformDirection =
			GenerateGIProbeFibonacciDirection(
				rayIndex,
				sequenceSampleCount,
				request.m_settings.m_randomSeed,
				0u);
		glm::vec3 direction{};
		float directionPdf = 0.0f;
		if (!sampler.SamplePrimaryDirection(
				uniformDirection,
				rayIndex,
				sequenceSampleCount,
				MixGIProbeRandomSeed(
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
		if (!Math::AllFinite(direction) ||
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
				position,
				direction,
				request.m_settings.m_maxRayDistance,
				MixGIProbeRandomSeed(
					request.m_settings.m_randomSeed,
					probeSeed,
					rayIndex,
					2u),
				sample,
				outDiagnostic))
		{
			return false;
		}

		const glm::vec3 radiance = Math::AllFinite(sample.m_radiance) ?
			glm::max(sample.m_radiance, glm::vec3(0.0f)) :
			glm::vec3(0.0f);
		const float projectionScale = 1.0f /
			(static_cast<float>(sequenceSampleCount) * directionPdf);
		float basis[GIProbeSphericalHarmonicsCoefficientCount]{};
		EvaluateSphericalHarmonicsBasis(direction, basis);
		for (uint32_t coefficientIndex = 0u;
			coefficientIndex < GIProbeSphericalHarmonicsCoefficientCount;
			++coefficientIndex)
		{
			workingAccumulator.m_weightedCoefficients[coefficientIndex] +=
				radiance * basis[coefficientIndex] * projectionScale *
				IrradianceConvolution(coefficientIndex);
		}
		++workingAccumulator.m_sampleCount;
	}
	accumulator = workingAccumulator;
	return true;
}

bool Sailor::ResolveGIProbeIrradiance(
	const GIProbeIrradianceAccumulator& accumulator,
	GIProbe& probe,
	std::string& outDiagnostic) noexcept
{
	outDiagnostic.clear();
	if (accumulator.m_sampleCount == 0u ||
		accumulator.m_sequenceSampleCount == 0u ||
		accumulator.m_sampleCount > accumulator.m_sequenceSampleCount)
	{
		outDiagnostic = "GI probe irradiance accumulator has no valid samples";
		return false;
	}
	const float partialSequenceScale =
		accumulator.m_sampleCount == accumulator.m_sequenceSampleCount ? 1.0f :
		static_cast<float>(accumulator.m_sequenceSampleCount) /
			static_cast<float>(accumulator.m_sampleCount);
	std::array<glm::vec3, GIProbeSphericalHarmonicsCoefficientCount>
		resolvedIrradiance{};
	for (uint32_t coefficientIndex = 0u;
		coefficientIndex < GIProbeSphericalHarmonicsCoefficientCount;
		++coefficientIndex)
	{
		const glm::vec3 value =
			accumulator.m_weightedCoefficients[coefficientIndex] *
			partialSequenceScale;
		if (!Math::AllFinite(value))
		{
			outDiagnostic =
				"GI probe irradiance accumulator resolved to a non-finite value";
			return false;
		}
		resolvedIrradiance[coefficientIndex] = value;
	}
	probe.m_irradiance = resolvedIrradiance;
	return true;
}
