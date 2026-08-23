#pragma once

#include "AssetRegistry/FileId.h"
#include "AssetRegistry/GlobalIllumination/ProbeVolumeData.h"
#include "Core/SpinLock.h"
#include "Memory/SharedPtr.hpp"
#include "Tasks/Tasks.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace YAML
{
	class Node;
}

namespace Sailor
{
	class World;

	SAILOR_SHARED_API bool AreWorldDocumentsEquivalentForProbeBake(
		const YAML::Node& savedDocument,
		const YAML::Node& currentDocument,
		std::string& outDiagnostic);

	enum class EEditorProbeVolumeBakeState : uint8_t
	{
		Idle = 0u,
		Preparing,
		Baking,
		Saving,
		Succeeded,
		Failed,
		Cancelled
	};

	struct SAILOR_SHARED_API EditorProbeVolumeBakeRequest final
	{
		FileId m_worldAsset{};
		std::string m_outputVirtualPath{};
		std::string m_stateName{};
		FileId m_layoutSource{};
		ProbeVolumeBakeSettings m_settings{};
		glm::vec3 m_volumeMin{};
		glm::vec3 m_volumeMax{};
		glm::vec3 m_fallbackEnvironment{ 0.03f };
		bool m_bAutoBounds = true;
		bool m_bOverwrite = false;
	};

	struct SAILOR_SHARED_API EditorProbeVolumeBakeStatus final
	{
		EEditorProbeVolumeBakeState m_state =
			EEditorProbeVolumeBakeState::Idle;
		float m_progress = 0.0f;
		uint32_t m_completedProbes = 0u;
		uint32_t m_totalProbes = 0u;
		uint32_t m_brickCount = 0u;
		uint32_t m_probeCount = 0u;
		float m_elapsedSeconds = 0.0f;
		uint64_t m_layoutHash = 0u;
		uint64_t m_transportHash = 0u;
		uint64_t m_lightingHash = 0u;
		std::string m_stage{};
		std::string m_outputVirtualPath{};
		std::string m_diagnostic{};

		bool IsRunning() const noexcept
		{
			return m_state == EEditorProbeVolumeBakeState::Preparing ||
				m_state == EEditorProbeVolumeBakeState::Baking ||
				m_state == EEditorProbeVolumeBakeState::Saving;
		}
	};

	class SAILOR_SHARED_API GlobalIlluminationBakeController final
	{
	public:
		GlobalIlluminationBakeController();
		~GlobalIlluminationBakeController();

		GlobalIlluminationBakeController(
			const GlobalIlluminationBakeController&) = delete;
		GlobalIlluminationBakeController& operator=(
			const GlobalIlluminationBakeController&) = delete;

		bool Start(
			World* world,
			const EditorProbeVolumeBakeRequest& request,
			std::string& outDiagnostic);
		bool Cancel(std::string& outDiagnostic);
		EditorProbeVolumeBakeStatus GetStatus() const;
		void Wait();

		// Shared with the single serialized Background task. Exposed only so
		// translation-unit helpers can update status without retaining Editor.
		struct SharedState final
		{
			mutable SpinLock m_lock;
			EditorProbeVolumeBakeStatus m_status{};
			std::atomic<bool> m_cancel{ false };
		};

	private:
		TSharedPtr<SharedState> m_state{};
		Tasks::ITaskPtr m_task{};
	};
}
