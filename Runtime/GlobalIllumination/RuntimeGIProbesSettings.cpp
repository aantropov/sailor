#include "GlobalIllumination/RuntimeGIProbesSettings.h"

#include "Core/YamlSerializable.h"
#include "GlobalIllumination/GIProbesData.h"

#include <cmath>

using namespace Sailor;

YAML::Node RuntimeGIProbesSettings::Serialize() const
{
	YAML::Node result(YAML::NodeType::Map);
	SERIALIZE_PROPERTY(result, m_version);
	::Serialize(result, "includeSky", m_bIncludeSky);
	::Serialize(result, "includeEmissive", m_bIncludeEmissive);
	::Serialize(result, "includeDirectLighting", m_bIncludeDirectLighting);
	SERIALIZE_PROPERTY(result, m_bounceCount);
	SERIALIZE_PROPERTY(result, m_minProbeSpacing);
	SERIALIZE_PROPERTY(result, m_normalBias);
	SERIALIZE_PROPERTY(result, m_viewBias);
	SERIALIZE_PROPERTY(result, m_maxRayDistance);
	return result;
}

bool RuntimeGIProbesSettings::Deserialize(const YAML::Node& inData)
{
	return DESERIALIZE_PROPERTY(inData, m_version) &&
		::Deserialize(inData, "includeSky", m_bIncludeSky) &&
		::Deserialize(inData, "includeEmissive", m_bIncludeEmissive) &&
		::Deserialize(inData, "includeDirectLighting", m_bIncludeDirectLighting) &&
		DESERIALIZE_PROPERTY(inData, m_bounceCount) &&
		DESERIALIZE_PROPERTY(inData, m_minProbeSpacing) &&
		DESERIALIZE_PROPERTY(inData, m_normalBias) &&
		DESERIALIZE_PROPERTY(inData, m_viewBias) &&
		DESERIALIZE_PROPERTY(inData, m_maxRayDistance);
}

GIProbesBakeSettings Sailor::ResolveRuntimeGIProbesBakeSettings(
	const RuntimeGIProbesSettings& settings,
	const RuntimeGIProbesQualitySettings& quality,
	uint32_t randomSeed) noexcept
{
	GIProbesBakeSettings result;
	result.m_raysPerProbe = quality.m_targetSamplesPerProbe;
	result.m_bounceCount = settings.m_bounceCount;
	result.m_randomSeed = randomSeed;
	result.m_maxSubdivisionLevel = 0u;
	result.m_minProbeSpacing = settings.m_minProbeSpacing *
		quality.m_spacingMultiplier;
	result.m_normalBias = settings.m_normalBias;
	result.m_viewBias = settings.m_viewBias;
	result.m_maxRayDistance = settings.m_maxRayDistance;
	result.m_bIncludeSky = settings.m_bIncludeSky;
	result.m_bIncludeEmissive = settings.m_bIncludeEmissive;
	result.m_bIncludeDirectLighting = settings.m_bIncludeDirectLighting;
	return result;
}

bool RuntimeGIProbesSettings::Validate(
	std::string& outDiagnostic) const noexcept
{
	outDiagnostic.clear();
	if (m_version != RuntimeGIProbesSettingsVersion)
	{
		outDiagnostic = "runtime GI probes settings version must be 1";
		return false;
	}
	if (m_bounceCount == 0u ||
		m_bounceCount > GIProbesMaxBounceCount)
	{
		outDiagnostic = "runtime GI probes bounce count is outside the supported range";
		return false;
	}
	if (!std::isfinite(m_minProbeSpacing) ||
		m_minProbeSpacing <= 0.0f ||
		!std::isfinite(m_normalBias) ||
		m_normalBias < 0.0f ||
		!std::isfinite(m_viewBias) ||
		m_viewBias < 0.0f ||
		!std::isfinite(m_maxRayDistance) ||
		m_maxRayDistance <= 0.0f)
	{
		outDiagnostic =
			"runtime GI probe spacing and ray distance must be positive; biases must be non-negative";
		return false;
	}
	return true;
}

bool RuntimeGIProbesQualitySettings::Validate(
	std::string& outDiagnostic) const noexcept
{
	outDiagnostic.clear();
	if (m_version != RuntimeGIProbesSettingsVersion)
	{
		outDiagnostic = "runtime GI probes quality settings version must be 1";
		return false;
	}
	if (m_maxActiveProbes < 8u ||
		m_maxActiveProbes > RuntimeGIProbesHardMaxActiveProbes)
	{
		outDiagnostic =
			"runtime GI probes capacity must be between 8 and 32768 probes";
		return false;
	}
	if (m_initialSamplesPerProbe < RuntimeGIProbesInitialSamplesPerProbe ||
		m_initialSamplesPerProbe > m_targetSamplesPerProbe ||
		m_targetSamplesPerProbe > GIProbesMaxRaysPerProbe)
	{
		outDiagnostic =
			"runtime GI probe samples must satisfy 16 <= initial <= target <= 65536";
		return false;
	}
	if (m_workerCount == 0u ||
		m_workerCount > RuntimeGIProbesHardMaxWorkerCount)
	{
		outDiagnostic = "runtime GI probes worker count must be between 1 and 16";
		return false;
	}
	if (m_maxDirtyUploadBytesPerFrame == 0u)
	{
		outDiagnostic = "runtime GI probes upload budget must be non-zero";
		return false;
	}
	if (!std::isfinite(m_spacingMultiplier) ||
		m_spacingMultiplier < 0.25f ||
		m_spacingMultiplier > 16.0f ||
		!std::isfinite(m_cpuDutyFraction) ||
		m_cpuDutyFraction <= 0.0f ||
		m_cpuDutyFraction > 1.0f ||
		!std::isfinite(m_cpuBudgetMilliseconds) ||
		m_cpuBudgetMilliseconds <= 0.0f ||
		m_cpuBudgetMilliseconds > 100.0f ||
		!std::isfinite(m_maxPublicationsPerSecond) ||
		m_maxPublicationsPerSecond <= 0.0f ||
		m_maxPublicationsPerSecond > 60.0f ||
		!std::isfinite(m_initialPublicationCoverage) ||
		m_initialPublicationCoverage <= 0.0f ||
		m_initialPublicationCoverage > 1.0f)
	{
		outDiagnostic = "runtime GI probes quality budgets are outside the supported range";
		return false;
	}
	return true;
}
