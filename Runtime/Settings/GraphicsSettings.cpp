#include "Settings/GraphicsSettings.h"

#include "RHI/Types.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
	using namespace Sailor;
	using namespace Sailor::Settings;

	size_t QualityIndex(EGraphicsQuality quality) noexcept
	{
		switch (quality)
		{
		case EGraphicsQuality::Ultra: return 0u;
		case EGraphicsQuality::High: return 1u;
		case EGraphicsQuality::Medium: return 2u;
		case EGraphicsQuality::Low: return 3u;
		case EGraphicsQuality::VeryLow: return 4u;
		default: return 1u;
		}
	}

	uint32_t ScaleDimension(uint32_t dimension, float scale) noexcept
	{
		if (dimension == 0u || !std::isfinite(scale) || scale <= 0.0f)
		{
			return 1u;
		}

		const double scaled = static_cast<double>(dimension) * static_cast<double>(scale);
		if (!std::isfinite(scaled) || scaled >= static_cast<double>((std::numeric_limits<uint32_t>::max)()))
		{
			return (std::numeric_limits<uint32_t>::max)();
		}
		return (std::max)(1u, static_cast<uint32_t>(std::floor(scaled + 0.5)));
	}
}

Sailor::Settings::GraphicsSettings::GraphicsSettings()
{
	auto& ultra = m_presets[QualityIndex(EGraphicsQuality::Ultra)];
	ultra.m_resolutionFactor = 1.0f;
	ultra.m_fpsCap = 120u;
	ultra.m_msaaSamples = 8u;
	ultra.m_shadowQuality = ELightShadowQuality::High;
	ultra.m_shadowBias = 1.25f;
	ultra.m_shadowCascadeCount = 4u;
	ultra.m_shadowCascadeResolutions = { 4096u, 2048u, 2048u, 1024u };
	ultra.m_bSupportSoftShadows = true;
	ultra.m_cloudsResolutionMultiplier = 1.0f;
	ultra.m_bCloudsDithering = false;
	ultra.m_skyResolution = 512u;
	ultra.m_vegetationInstanceBudget = 16384u;
	ultra.m_lodBias = -1;
	ultra.m_maxGiProbeStatesPerSnapshot = 4u;
	ultra.m_runtimeGIProbes.m_maxActiveProbes = 16384u;
	ultra.m_runtimeGIProbes.m_cpuDutyFraction = 0.5f;

	auto& high = m_presets[QualityIndex(EGraphicsQuality::High)];
	high.m_resolutionFactor = 1.0f;
	high.m_fpsCap = 120u;
	high.m_msaaSamples = 4u;
	high.m_shadowQuality = ELightShadowQuality::High;
	high.m_shadowBias = 1.25f;
	high.m_shadowCascadeCount = 4u;
	high.m_shadowCascadeResolutions = { 2048u, 2048u, 1024u, 1024u };
	high.m_bSupportSoftShadows = true;
	high.m_cloudsResolutionMultiplier = 0.75f;
	high.m_bCloudsDithering = false;
	high.m_skyResolution = 256u;
	high.m_vegetationInstanceBudget = 8192u;
	high.m_lodBias = 0;
	high.m_maxGiProbeStatesPerSnapshot = 3u;

	auto& medium = m_presets[QualityIndex(EGraphicsQuality::Medium)];
	medium.m_resolutionFactor = 0.85f;
	medium.m_fpsCap = 120u;
	medium.m_msaaSamples = 2u;
	medium.m_shadowQuality = ELightShadowQuality::Medium;
	medium.m_shadowBias = 1.25f;
	medium.m_shadowCascadeCount = 3u;
	medium.m_shadowCascadeResolutions = { 2048u, 1024u, 512u, 0u };
	medium.m_bSupportSoftShadows = true;
	medium.m_cloudsResolutionMultiplier = 0.5f;
	medium.m_bCloudsDithering = false;
	medium.m_skyResolution = 256u;
	medium.m_vegetationInstanceBudget = 4096u;
	medium.m_lodBias = 0;
	medium.m_maxGiProbeStatesPerSnapshot = 2u;
	medium.m_runtimeGIProbes.m_maxActiveProbes = 4096u;
	medium.m_runtimeGIProbes.m_cpuDutyFraction = 0.2f;
	medium.m_runtimeGIProbes.m_maxPublicationsPerSecond = 6.0f;

	auto& low = m_presets[QualityIndex(EGraphicsQuality::Low)];
	low.m_resolutionFactor = 0.7f;
	low.m_fpsCap = 120u;
	low.m_msaaSamples = 1u;
	low.m_shadowQuality = ELightShadowQuality::Low;
	low.m_shadowBias = 1.25f;
	low.m_shadowCascadeCount = 2u;
	low.m_shadowCascadeResolutions = { 1024u, 512u, 0u, 0u };
	low.m_bSupportSoftShadows = false;
	low.m_cloudsResolutionMultiplier = 0.25f;
	low.m_bCloudsDithering = false;
	low.m_skyResolution = 128u;
	low.m_vegetationInstanceBudget = 2048u;
	low.m_lodBias = 1;
	low.m_maxGiProbeStatesPerSnapshot = 2u;
	low.m_runtimeGIProbes.m_maxActiveProbes = 2048u;
	low.m_runtimeGIProbes.m_targetSamplesPerProbe = 32u;
	low.m_runtimeGIProbes.m_cpuDutyFraction = 0.15f;
	low.m_runtimeGIProbes.m_maxPublicationsPerSecond = 4.0f;

	auto& veryLow = m_presets[QualityIndex(EGraphicsQuality::VeryLow)];
	veryLow.m_resolutionFactor = 0.5f;
	veryLow.m_fpsCap = 120u;
	veryLow.m_msaaSamples = 1u;
	veryLow.m_shadowQuality = ELightShadowQuality::VeryLow;
	veryLow.m_shadowBias = 1.25f;
	veryLow.m_shadowCascadeCount = 1u;
	veryLow.m_shadowCascadeResolutions = { 512u, 0u, 0u, 0u };
	veryLow.m_bSupportSoftShadows = false;
	veryLow.m_cloudsResolutionMultiplier = 0.125f;
	veryLow.m_bCloudsDithering = false;
	veryLow.m_skyResolution = 64u;
	veryLow.m_vegetationInstanceBudget = 512u;
	veryLow.m_lodBias = 2;
	veryLow.m_maxGiProbeStatesPerSnapshot = 1u;
	veryLow.m_runtimeGIProbes.m_maxActiveProbes = 2048u;
	veryLow.m_runtimeGIProbes.m_targetSamplesPerProbe = 16u;
	veryLow.m_runtimeGIProbes.m_cpuDutyFraction = 0.1f;
	veryLow.m_runtimeGIProbes.m_maxPublicationsPerSecond = 2.0f;
}

bool Sailor::Settings::GraphicsQualityProfile::IsShadowCascadeActive(
	uint32_t cascadeIndex) const noexcept
{
	return cascadeIndex < m_shadowCascadeCount && cascadeIndex < MaxShadowCascades;
}

uint32_t Sailor::Settings::GraphicsQualityProfile::GetShadowCascadeResolution(
	uint32_t cascadeIndex) const noexcept
{
	return IsShadowCascadeActive(cascadeIndex) ? m_shadowCascadeResolutions[cascadeIndex] : 0u;
}

const Sailor::Settings::GraphicsQualityProfile& Sailor::Settings::GraphicsSettings::GetProfile(
	EGraphicsQuality quality) const noexcept
{
	return m_presets[QualityIndex(quality)];
}

const Sailor::Settings::GraphicsQualityProfile& Sailor::Settings::GraphicsSettingsState::GetActiveProfile() const noexcept
{
	return m_projectSettings.GetProfile(m_activeQuality);
}

Sailor::Settings::EGraphicsQuality Sailor::Settings::ResolveQualitySelection(
	EGraphicsQualitySelection selection,
	EGraphicsQuality projectDefault) noexcept
{
	switch (selection)
	{
	case EGraphicsQualitySelection::Ultra: return EGraphicsQuality::Ultra;
	case EGraphicsQualitySelection::High: return EGraphicsQuality::High;
	case EGraphicsQualitySelection::Medium: return EGraphicsQuality::Medium;
	case EGraphicsQualitySelection::Low: return EGraphicsQuality::Low;
	case EGraphicsQualitySelection::VeryLow: return EGraphicsQuality::VeryLow;
	case EGraphicsQualitySelection::ProjectDefault:
	default:
		return projectDefault;
	}
}

Sailor::RHI::EMsaaSamples Sailor::Settings::ToMsaaSamples(uint32_t samples) noexcept
{
	switch (samples)
	{
	case 2u: return RHI::EMsaaSamples::Samples_2;
	case 4u: return RHI::EMsaaSamples::Samples_4;
	case 8u: return RHI::EMsaaSamples::Samples_8;
	case 1u:
	default:
		return RHI::EMsaaSamples::Samples_1;
	}
}

Sailor::Settings::GraphicsExtent Sailor::Settings::ResolveRenderDimensions(
	uint32_t outputWidth,
	uint32_t outputHeight,
	float resolutionFactor) noexcept
{
	return
	{
		ScaleDimension(outputWidth, resolutionFactor),
		ScaleDimension(outputHeight, resolutionFactor)
	};
}

Sailor::Settings::GraphicsExtent Sailor::Settings::ResolveSkyExtent(
	const GraphicsQualityProfile& profile) noexcept
{
	const uint32_t resolution = (std::max)(1u, profile.m_skyResolution);
	return { resolution, resolution };
}

Sailor::Settings::GraphicsExtent Sailor::Settings::ResolveCloudsExtent(
	uint32_t renderWidth,
	uint32_t renderHeight,
	const GraphicsQualityProfile& profile,
	float platformMultiplier) noexcept
{
	const uint32_t shortestDimension = (std::min)(renderWidth, renderHeight);
	const float scale = profile.m_cloudsResolutionMultiplier * platformMultiplier;
	const uint32_t resolution = ScaleDimension(shortestDimension, scale);
	return { resolution, resolution };
}

Sailor::ELightShadowQuality Sailor::Settings::ApplyShadowQualityCap(
	ELightShadowQuality authoredQuality,
	ELightShadowQuality qualityCap) noexcept
{
	const uint8_t highestQuality = static_cast<uint8_t>(ELightShadowQuality::High);
	const uint8_t authored = (std::min)(static_cast<uint8_t>(authoredQuality), highestQuality);
	const uint8_t cap = (std::min)(static_cast<uint8_t>(qualityCap), highestQuality);
	return static_cast<ELightShadowQuality>((std::min)(authored, cap));
}

uint32_t Sailor::Settings::ApplyLodBias(
	uint32_t selectedLod,
	uint32_t numAvailableLods,
	uint32_t authoredMinLod,
	uint32_t authoredMaxLod,
	int32_t lodBias) noexcept
{
	if (numAvailableLods == 0u)
	{
		return 0u;
	}

	const uint32_t highestAvailableLod = numAvailableLods - 1u;
	const uint32_t minLod = (std::min)(authoredMinLod, highestAvailableLod);
	const uint32_t maxLod = (std::max)(minLod, (std::min)(authoredMaxLod, highestAvailableLod));
	const int64_t selected = static_cast<int64_t>((std::clamp)(selectedLod, minLod, maxLod));
	const int64_t biased = selected + static_cast<int64_t>(lodBias);
	return static_cast<uint32_t>((std::clamp)(
		biased,
		static_cast<int64_t>(minLod),
		static_cast<int64_t>(maxLod)));
}
