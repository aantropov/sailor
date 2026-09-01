#pragma once

#include "GlobalIllumination/GIProbesBaker.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace Sailor
{
	struct SAILOR_SHARED_API GIProbeIrradianceAccumulator final
	{
		std::array<glm::vec3, GIProbeSphericalHarmonicsCoefficientCount>
			m_weightedCoefficients{};
		uint32_t m_sampleCount = 0u;
		uint32_t m_sequenceSampleCount = 0u;

		void Reset() noexcept;
	};

	struct SAILOR_SHARED_API GIProbeTraceRequest final
	{
		GIProbesBakeSettings m_settings{};
		glm::vec3 m_volumeMin{};
		glm::vec3 m_volumeMax{};
		const std::atomic<bool>* m_cancel = nullptr;
	};

	SAILOR_SHARED_API uint32_t MixGIProbeRandomSeed(
		uint32_t baseSeed,
		uint32_t probeSeed,
		uint32_t sampleIndex,
		uint32_t stream) noexcept;

	SAILOR_SHARED_API glm::vec3 GenerateGIProbeFibonacciDirection(
		uint32_t index,
		uint32_t count,
		uint32_t seed,
		uint32_t probeSeed) noexcept;

	SAILOR_SHARED_API bool TraceGIProbeTransport(
		const GIProbeTraceRequest& request,
		const IGIProbeBakeRaySampler& sampler,
		uint32_t probeSeed,
		float visibilityMaxDistance,
		GIProbe& probe,
		std::string& outDiagnostic);

	SAILOR_SHARED_API bool AccumulateGIProbeIrradianceRange(
		const GIProbeTraceRequest& request,
		const IGIProbeBakeRaySampler& sampler,
		const glm::vec3& position,
		uint32_t probeSeed,
		uint32_t sampleBegin,
		uint32_t sampleCount,
		uint32_t sequenceSampleCount,
		GIProbeIrradianceAccumulator& accumulator,
		std::string& outDiagnostic);

	SAILOR_SHARED_API bool ResolveGIProbeIrradiance(
		const GIProbeIrradianceAccumulator& accumulator,
		GIProbe& probe,
		std::string& outDiagnostic) noexcept;
}
