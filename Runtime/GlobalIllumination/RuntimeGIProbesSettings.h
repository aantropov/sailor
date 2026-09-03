#pragma once

#include "Core/Defines.h"

#include <cstdint>
#include <string>

namespace Sailor
{
	struct GIProbesBakeSettings;

	inline constexpr uint32_t RuntimeGIProbesSettingsVersion = 1u;
	inline constexpr uint32_t RuntimeGIProbesHardMaxActiveProbes = 32768u;
	inline constexpr uint32_t RuntimeGIProbesHardMaxWorkerCount = 16u;
	inline constexpr uint32_t RuntimeGIProbesInitialSamplesPerProbe = 16u;

	struct SAILOR_SHARED_API RuntimeGIProbesSettings final
	{
		uint32_t m_version = RuntimeGIProbesSettingsVersion;
		uint32_t m_bounceCount = 3u;
		float m_minProbeSpacing = 1.0f;
		float m_normalBias = 0.05f;
		float m_viewBias = 0.05f;
		float m_maxRayDistance = 1000.0f;
		bool m_bIncludeSky = true;
		bool m_bIncludeEmissive = true;
		bool m_bIncludeDirectLighting = true;

		bool operator==(const RuntimeGIProbesSettings&) const noexcept = default;
		bool Validate(std::string& outDiagnostic) const noexcept;
	};

	struct SAILOR_SHARED_API RuntimeGIProbesQualitySettings final
	{
		uint32_t m_version = RuntimeGIProbesSettingsVersion;
		uint32_t m_maxActiveProbes = 8192u;
		uint32_t m_initialSamplesPerProbe =
			RuntimeGIProbesInitialSamplesPerProbe;
		uint32_t m_targetSamplesPerProbe = 64u;
		uint32_t m_workerCount = 2u;
		uint32_t m_maxDirtyUploadBytesPerFrame = 2u * 1024u * 1024u;
		float m_spacingMultiplier = 1.0f;
		float m_cpuDutyFraction = 0.25f;
		float m_cpuBudgetMilliseconds = 4.0f;
		float m_maxPublicationsPerSecond = 8.0f;
		float m_initialPublicationCoverage = 0.125f;
		bool m_bEnabled = false;

		bool operator==(
			const RuntimeGIProbesQualitySettings&) const noexcept = default;
		bool Validate(std::string& outDiagnostic) const noexcept;
	};

	SAILOR_SHARED_API GIProbesBakeSettings ResolveRuntimeGIProbesBakeSettings(
		const RuntimeGIProbesSettings& settings,
		const RuntimeGIProbesQualitySettings& quality,
		uint32_t randomSeed) noexcept;
}
