#include "Editor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Audio/AudioImporter.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"
#include "Audio/AudioSystem.h"
#include "Containers/Map.h"
#include "Core/LogMacros.h"
#include "Editor/EditorViewportController.h"
#include "Editor/GlobalIlluminationBakeController.h"
#include "Engine/EngineLoop.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "YamlExceptionBoundary.h"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>

using namespace Sailor;

namespace Sailor
{
	struct EditorManagedMutationState
	{
		TMap<InstanceId, uint64_t> m_objectRevisions{};
	};
}

namespace
{
	constexpr uint64_t c_primaryEditorViewportId = 1u;
}

Editor::Editor(Platform::NativeWindow* pMainWindow) :
	m_pMainWindow(pMainWindow),
	m_viewportController(TUniquePtr<EditorViewport::EditorViewportController>::Make()),
	m_giProbesBakeController(TUniquePtr<GlobalIlluminationBakeController>::Make()),
	m_managedMutationState(TUniquePtr<EditorManagedMutationState>::Make())
{

}

Editor::~Editor()
{
	std::string diagnostic;
	m_giProbesBakeController->Cancel(diagnostic);
	m_giProbesBakeController->Wait();
	StopAudioPreview();
}

bool Editor::PreviewAudioAsset(const FileId& fileId)
{
	StopAudioPreview();

	auto* assetRegistry = App::GetSubmodule<AssetRegistry>();
	auto* audioSystem = App::GetSubmodule<AudioSystem>();
	if (!fileId || !assetRegistry || !audioSystem || !audioSystem->IsInitialized())
	{
		return false;
	}

	const AudioClipPtr clip = assetRegistry->LoadAssetFromFile<AudioClip>(fileId, true);
	AudioVoiceId voiceId = InvalidAudioVoiceId;
	if (!clip || !audioSystem->CreateVoice(clip, voiceId))
	{
		return false;
	}

	AudioVoiceSettings settings{};
	settings.m_bLoop = false;
	settings.m_bSpatial = false;
	if (!audioSystem->SetVoiceSettings(voiceId, settings) ||
		!audioSystem->PlayVoice(voiceId, true))
	{
		audioSystem->DestroyVoice(voiceId);
		return false;
	}

	m_audioPreviewVoiceId = voiceId;
	return true;
}

void Editor::StopAudioPreview()
{
	if (m_audioPreviewVoiceId == InvalidAudioVoiceId)
	{
		return;
	}

	if (auto* audioSystem = App::GetSubmodule<AudioSystem>())
	{
		audioSystem->StopVoice(m_audioPreviewVoiceId);
		audioSystem->DestroyVoice(m_audioPreviewVoiceId);
	}
	m_audioPreviewVoiceId = InvalidAudioVoiceId;
}

void Editor::SetWorld(World* world)
{
	std::string diagnostic;
	m_giProbesBakeController->Cancel(diagnostic);
	m_giProbesBakeController->Wait();
	m_world = world;
	std::string{}.swap(m_simulationSnapshot);
	m_managedSelectionMutationRevision = 0;
	m_managedMutationState->m_objectRevisions.Clear();
	m_viewportController->Reset();
	m_viewportController->SetManagedMutationRevisions(0, 0);
}

bool Editor::StartGIProbesBake(
	const EditorGIProbesBakeRequest& request,
	std::string& outDiagnostic)
{
	return m_giProbesBakeController &&
		m_giProbesBakeController->Start(m_world, request, outDiagnostic);
}

bool Editor::CancelGIProbesBake(std::string& outDiagnostic)
{
	return m_giProbesBakeController &&
		m_giProbesBakeController->Cancel(outDiagnostic);
}

EditorGIProbesBakeStatus Editor::GetGIProbesBakeStatus() const
{
	return m_giProbesBakeController
		? m_giProbesBakeController->GetStatus()
		: EditorGIProbesBakeStatus{};
}

bool Editor::SetSimulationEnabled(bool bEnabled)
{
	if (bEnabled == IsSimulationEnabled())
	{
		return true;
	}

	if (!m_world)
	{
		return false;
	}

	if (bEnabled)
	{
		std::string snapshot;
		std::string diagnostic;
		if (!External::TryDumpYaml(SerializeWorld(), snapshot, diagnostic) || snapshot.empty())
		{
			SAILOR_LOG_ERROR("Cannot start Editor simulation: %s.", diagnostic.empty()
				? "the current world could not be snapshotted" : diagnostic.c_str());
			return false;
		}

		// Retain compact text, not a live YAML tree or mutable prefab baseline.
		CancelViewportInteraction();
		m_simulationSnapshot = std::move(snapshot);
		m_world->SetPhysicsSimulationEnabled(true);
		return true;
	}

	auto* engineLoop = App::GetSubmodule<EngineLoop>();
	auto* worldImporter = App::GetSubmodule<WorldPrefabImporter>();
	if (!engineLoop || !worldImporter)
	{
		return false;
	}

	YAML::Node snapshot;
	std::string diagnostic;
	if (!External::TryLoadYaml(m_simulationSnapshot, snapshot, diagnostic))
	{
		SAILOR_LOG_ERROR("Cannot stop Editor simulation: the world snapshot is invalid: %s.", diagnostic.c_str());
		return false;
	}

	auto worldPrefab = worldImporter->Create();
	if (!worldPrefab ||
		!External::GuardYamlExceptions(
			[&worldPrefab, &snapshot]()
			{
				worldPrefab->Deserialize(snapshot);
			},
			diagnostic) ||
		!worldPrefab->IsReady())
	{
		if (diagnostic.empty() && worldPrefab)
		{
			diagnostic = worldPrefab->GetLoadDiagnostic();
		}
		SAILOR_LOG_ERROR(
			"Cannot stop Editor simulation: the world snapshot could not be restored: %s.",
			diagnostic.empty() ? "unknown error" : diagnostic.c_str());
		return false;
	}

	TVector<InstanceId> selection;
	selection.Reserve(m_world->m_editorSelection.Num());
	for (const InstanceId& instanceId : m_world->m_editorSelection)
	{
		selection.Add(instanceId);
	}

	World* oldWorld = m_world;
	auto restoredWorld = engineLoop->InstantiateWorld(
		worldPrefab,
		EngineLoop::EditorWorldMask);
	if (!restoredWorld)
	{
		SAILOR_LOG_ERROR(
			"Cannot stop Editor simulation: the world snapshot could not be instantiated.");
		return false;
	}

	SetWorld(restoredWorld.GetRawPtr());
	m_world->SetEditorSelection(selection);
	engineLoop->ExitWorld(oldWorld);
	engineLoop->ProcessPendingWorldExits();
	return true;
}

void Editor::TickViewportTools()
{
	if (m_world)
	{
		uint64_t selectedObjectRevision = 0;
		if (const auto gameObject = m_world->GetPrimaryEditorSelection())
		{
			selectedObjectRevision = GetManagedObjectMutationRevision(gameObject->GetInstanceId());
		}

		m_viewportController->SetManagedMutationRevisions(
			m_managedSelectionMutationRevision,
			selectedObjectRevision);
#if defined(_WIN32)
		if (m_pMainWindow)
		{
			std::string fileId{};
			float normalizedX = 0.0f;
			float normalizedY = 0.0f;
			while (m_pMainWindow->PullEditorViewportAssetDrop(
				fileId,
				normalizedX,
				normalizedY))
			{
				m_viewportController->QueueAssetDropEvent(
					fileId,
					normalizedX,
					normalizedY);
			}

			uint32_t shortcutKeyCode = 0;
			while (m_pMainWindow->PullEditorViewportToolShortcut(
				shortcutKeyCode))
			{
				m_viewportController->QueueToolShortcutEvent(
					shortcutKeyCode);
			}
		}
#endif
		m_viewportController->Tick(*m_world);
	}
}

void Editor::NotifyManagedObjectMutation(const InstanceId& instanceId)
{
	const InstanceId gameObjectId = instanceId.GameObjectId();
	if (gameObjectId)
	{
		++m_managedMutationState->m_objectRevisions[gameObjectId];
	}
}

uint64_t Editor::GetManagedObjectMutationRevision(const InstanceId& instanceId) const
{
	const InstanceId gameObjectId = instanceId.GameObjectId();
	const uint64_t* revision = nullptr;
	return gameObjectId && m_managedMutationState->m_objectRevisions.Find(gameObjectId, revision)
		? *revision
		: 0;
}

void Editor::CancelViewportInteraction()
{
	if (m_world)
	{
		m_viewportController->CancelInteraction(*m_world);
	}
	else
	{
		m_viewportController->CancelPointerInteraction();
	}
}

bool Editor::PullViewportEvent(EditorViewport::Event& outEvent)
{
	return m_viewportController->PullEvent(outEvent);
}

void Editor::ShowMainWindow(bool bShow)
{
	m_pMainWindow->Show(bShow);
}

bool Editor::TraceViewportRay(
	uint64_t viewportId,
	float normalizedX,
	float normalizedY,
	glm::vec3& outPosition) const
{
	return viewportId == c_primaryEditorViewportId &&
		m_world &&
		m_viewportController &&
		m_viewportController->TraceViewportRay(
			*m_world,
			normalizedX,
			normalizedY,
			outPosition);
}

bool Editor::FocusEditorCamera(const InstanceId& instanceId)
{
	return m_world &&
		m_viewportController &&
		m_viewportController->FocusCameraOnObject(*m_world, instanceId);
}

bool Editor::SetViewportToolState(
	EditorViewport::ETransformOperation operation,
	EditorViewport::ETransformSpace space)
{
	return m_viewportController &&
		m_viewportController->SetTransformToolState(operation, space);
}

void Editor::GetViewportToolState(
	EditorViewport::ETransformOperation& outOperation,
	EditorViewport::ETransformSpace& outSpace) const
{
	outOperation = m_viewportController
		? m_viewportController->GetOperation()
		: EditorViewport::ETransformOperation::Translate;
	outSpace = m_viewportController
		? m_viewportController->GetSpace()
		: EditorViewport::ETransformSpace::World;
}

void Editor::PushMessage(std::string_view msg)
{
	std::time_t now = std::time(nullptr);
	std::tm localTime;

#if defined(_WIN32)
	errno_t err = localtime_s(&localTime, &now);
	if (err != 0)
	{
		return;
	}
#else
	if (!localtime_r(&now, &localTime))
	{
		return;
	}
#endif

	std::ostringstream oss;
	oss << '[' << std::put_time(&localTime, "%H:%M:%S") << "] " << msg;

	size_t numMessages = m_numMessages.load(std::memory_order_relaxed);
	do
	{
		if (numMessages >= 1024)
		{
			return;
		}
	}
	while (!m_numMessages.compare_exchange_weak(
		numMessages,
		numMessages + 1,
		std::memory_order_relaxed));

	m_messagesQueue.push(oss.str());
}

bool Editor::PullMessage(std::string& msg)
{
	if (m_messagesQueue.try_pop(msg))
	{
		m_numMessages.fetch_sub(1, std::memory_order_relaxed);
		return true;
	}

	return false;
}

YAML::Node Editor::SerializeWorld() const
{
	SAILOR_PROFILE_FUNCTION();

	if (m_world == nullptr)
	{
		return YAML::Node();
	}

	auto prefab = WorldPrefab::FromWorld(m_world);
	if (!prefab || !prefab->IsReady())
	{
		SAILOR_LOG_ERROR(
			"Cannot serialize the current world: %s.",
			prefab
				? prefab->GetLoadDiagnostic().c_str()
				: "world serialization did not create a document");
		return YAML::Node();
	}

	return prefab->Serialize();
}
