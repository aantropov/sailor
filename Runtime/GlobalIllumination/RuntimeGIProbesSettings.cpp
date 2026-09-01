#include "GlobalIllumination/RuntimeGIProbesSettings.h"

#include "GlobalIllumination/GIProbesData.h"

#include <cmath>

using namespace Sailor;

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
		outDiagnostic = "runtime GI probes authored distances must be finite and non-negative";
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
	if (m_clipmapCascadeCount == 0u || m_clipmapCascadeCount > 4u)
	{
		outDiagnostic = "runtime GI probes clipmap cascade count must be between 1 and 4";
		return false;
	}
	if (m_maxActiveProbes < m_clipmapCascadeCount * 8u ||
		m_maxActiveProbes > RuntimeGIProbesHardMaxActiveProbes)
	{
		outDiagnostic =
			"runtime GI probes capacity must hold at least eight probes per clipmap cascade and at most 32768 probes";
		return false;
	}
	if (m_initialSamplesPerProbe == 0u ||
		m_initialSamplesPerProbe > m_targetSamplesPerProbe ||
		m_targetSamplesPerProbe > GIProbesMaxRaysPerProbe)
	{
		outDiagnostic = "runtime GI probes sample thresholds are invalid";
		return false;
	}
	if (m_workerCount == 0u ||
		m_workerCount > RuntimeGIProbesHardMaxWorkerCount)
	{
		outDiagnostic = "runtime GI probes worker count must be 1 or 2";
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
