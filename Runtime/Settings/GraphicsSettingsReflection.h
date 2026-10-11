#pragma once

#include "Settings/GraphicsSettings.h"
#include "Core/YamlSerializable.h"

REFL_AUTO(
	type(Sailor::RuntimeGIProbesQualitySettings),
	field(m_version),
	field(m_bEnabled, Sailor::Attributes::YamlName("enabled")),
	field(m_maxActiveProbes),
	field(m_initialSamplesPerProbe),
	field(m_targetSamplesPerProbe),
	field(m_workerCount),
	field(m_maxDirtyUploadBytesPerFrame),
	field(m_spacingMultiplier),
	field(m_cpuDutyFraction),
	field(m_cpuBudgetMilliseconds),
	field(m_maxPublicationsPerSecond),
	field(m_initialPublicationCoverage)
)

REFL_AUTO(
	type(Sailor::Settings::GraphicsQualityProfile),
	field(m_resolutionFactor),
	field(m_fpsCap),
	field(m_msaaSamples),
	field(m_shadowQuality),
	field(m_shadowBias),
	field(m_shadowDistance),
	field(m_shadowCascadeCount),
	field(m_bSupportSoftShadows, Sailor::Attributes::YamlName("supportSoftShadows")),
	field(m_cloudsResolutionMultiplier),
	field(m_bCloudsDithering, Sailor::Attributes::YamlName("cloudsDithering")),
	field(m_skyResolution),
	field(m_vegetationInstanceBudget, Sailor::Attributes::YamlOptional{}),
	field(m_lodBias),
	field(m_bEnableGlobalIllumination, Sailor::Attributes::YamlName("enableGlobalIllumination")),
	field(m_maxGiProbeStatesPerSnapshot),
	field(m_runtimeGIProbes)
)

REFL_AUTO(
	type(Sailor::Settings::EditorGraphicsSettings),
	field(m_selectedQuality),
	field(m_statsMode),
	field(m_bRuntimeGIProbesPreviewEnabled, Sailor::Attributes::YamlName("runtimeGIProbesPreviewEnabled")),
	field(m_runtimeGIProbesBudget),
	field(m_runtimeGIProbesDebugView)
)

namespace YAML
{
	// Only active cascades are written; the runtime keeps a fixed-size array.
	template<>
	struct SAILOR_SHARED_API convert<Sailor::Settings::GraphicsQualityProfile>
	{
		static Node encode(const Sailor::Settings::GraphicsQualityProfile& profile);
		static bool decode(const Node& node, Sailor::Settings::GraphicsQualityProfile& profile);
	};
}
