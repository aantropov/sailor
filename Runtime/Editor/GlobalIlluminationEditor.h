#pragma once

#include "ECS/GlobalIlluminationECS.h"
#include "Settings/GraphicsSettings.h"

#include <cstdint>
#include <string>

namespace Sailor
{
	struct EditorGIProbesBakeRequest;
	struct EditorGIProbesBakeStatus;

	struct SAILOR_SHARED_API EditorGlobalIlluminationState final
	{
		uint32_t m_maxProbeStatesPerSnapshot = 0u;
		EGlobalIlluminationMode m_mode =
			EGlobalIlluminationMode::Baked;
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

	namespace EditorRuntime
	{
		SAILOR_API bool StartEditorGIProbesBake(
			const EditorGIProbesBakeRequest& request,
			std::string& outDiagnostic);
		SAILOR_API bool CancelEditorGIProbesBake(std::string& outDiagnostic);
		SAILOR_API bool GetEditorGIProbesBakeStatus(EditorGIProbesBakeStatus& outStatus);
		SAILOR_API bool SetEditorGISettings(GISettings settings, std::string& outDiagnostic);
		SAILOR_API bool GetEditorGlobalIlluminationState(EditorGlobalIlluminationState& outState);
		SAILOR_API bool SetEditorRuntimeGIProbesPreviewEnabled(bool bEnabled, std::string& outDiagnostic);
		SAILOR_API bool SetEditorRuntimeGIProbesBudget(
			Settings::ERuntimeGIProbesEditorBudget budget,
			std::string& outDiagnostic);
		SAILOR_API bool SetEditorRuntimeGIProbesPaused(bool bPaused, std::string& outDiagnostic);
		SAILOR_API bool RestartEditorRuntimeGIProbes(std::string& outDiagnostic);
		SAILOR_API bool RebuildEditorRuntimeGIProbesScene(std::string& outDiagnostic);
	}
}
