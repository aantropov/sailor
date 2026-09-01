#pragma once

#include "ECS/GlobalIlluminationECS.h"
#include "Settings/GraphicsSettings.h"

#include <cstdint>
#include <string>

namespace Sailor
{
	struct SAILOR_SHARED_API EditorGlobalIlluminationState final
	{
		uint32_t m_maxProbeStatesPerSnapshot = 0u;
		EGlobalIlluminationMode m_mode =
			EGlobalIlluminationMode::RealtimeAndBaked;
		EGlobalIlluminationProbeSource m_probeSource =
			EGlobalIlluminationProbeSource::BakedAssets;
		RuntimeGIProbesSettings m_runtimeSettings{};
		RuntimeGIProbesStatus m_runtimeStatus{};
		bool m_bRuntimePreviewEnabled = false;
		Settings::ERuntimeGIProbesEditorBudget m_runtimeEditorBudget =
			Settings::ERuntimeGIProbesEditorBudget::Eco;
		bool m_bEnabled = true;
		TVector<GlobalIlluminationProbeState> m_probes{};
		std::string m_diagnostic{};
		uint64_t m_compositionCount = 0u;
		uint64_t m_rejectedCompositionCount = 0u;
	};
}
