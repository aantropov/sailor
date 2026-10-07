#pragma once
#include "Core/Defines.h"
#include "Core/LogMacros.h"
#include "Core/Utils.h"
#include "Core/Submodule.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "Memory/UniquePtr.hpp"
#include "Platform/NativeWindow.h"
#include "Containers/Containers.h"
#include "Math/Math.h"
#include "RHI/RenderDebugView.h"
#include "Settings/GraphicsSettings.h"
#include "Workspace/WorkspaceContext.h"
#include <functional>

namespace Sailor
{
	namespace Tests { class TaskTestApp; }

	namespace Workspace
	{
		class WorkspaceModuleManager;
	}
	namespace Tasks
	{
		class ITask;
		class Scheduler;
	}

	struct AppArgs
	{
		bool m_bWaitForDebugger = false;
		bool m_bRunConsole = true;
		bool m_bUpdateWindowTitle = true;
		bool m_bIsEditor = false;
		bool m_bEnableRenderValidationLayers = true;
		bool m_bRunPathTracer = false;
		bool m_bForceNullAudioDevice = false;

		uint32_t m_editorPort = 32800;
		HWND m_editorHwnd{};
		std::string m_workspace;
		std::string m_workspaceManifest;

		// TODO: Change the default scene to empty?
		std::string m_world = "Editor.world";
	};

	enum class EAppInitializationResult : uint8_t
	{
		Failed,
		Ready,
		Completed
	};

	class App
	{
		static constexpr size_t MaxSubmodules = 128u;
		static std::string s_workspace;

	public:

		SAILOR_API static App* GetInstance();
		SAILOR_API static const std::string& GetWorkspace();
		SAILOR_API static const Workspace::WorkspaceContext& GetWorkspaceContext();
		SAILOR_API static const Settings::GraphicsSettings& GetGraphicsSettings();
		SAILOR_API static const Settings::GraphicsQualityProfile& GetActiveGraphicsSettings();
		SAILOR_API static Settings::EGraphicsQualitySelection GetSelectedGraphicsQuality();
		SAILOR_API static const Settings::EditorGraphicsSettings& GetEditorGraphicsSettings();
		SAILOR_API static Settings::EGraphicsQuality GetActiveGraphicsQuality();
		SAILOR_API static Settings::ERenderStatsMode GetRenderStatsMode();
		SAILOR_API static bool SetRenderStatsMode(Settings::ERenderStatsMode mode);
		SAILOR_API static RHI::ESceneViewRenderMode GetEditorRenderMode();
		SAILOR_API static bool SetEditorRenderMode(RHI::ESceneViewRenderMode mode);

		SAILOR_API static EAppInitializationResult Initialize(const char** commandLineArgs = nullptr, int32_t num = 0);
		SAILOR_API static void Start();
		SAILOR_API static void Stop();
		SAILOR_API static bool Shutdown();
		SAILOR_API static bool IsEngineMainThreadReady();

		// Synchronous call; returns the fallback when lifecycle admission is closed.
		template<typename TResult>
		static TResult ExecuteOnEngineMainThread(TResult fallback, std::function<TResult()> command)
		{
			TResult result = fallback;
			if (!command || !DispatchOnEngineMainThread([&result, &command]()
				{
					result = command();
				}))
			{
				return fallback;
			}

			return result;
		}

		SAILOR_API static bool RequestAssetReload();
		SAILOR_API static bool UpdateAsset(const char* strFileId, bool bReimport = false);
		SAILOR_API static bool GetAssetReloadState(
			uint64_t& outRequestGeneration,
			uint64_t& outCompletedGeneration,
			uint64_t& outSuccessfulGeneration);
		SAILOR_API static bool IsRendererInitialized();
		SAILOR_API static bool HasEditor();
		SAILOR_API static bool IsEditorMode();
		SAILOR_API static uint32_t PullEditorMessages(char** messages, uint32_t num);
		SAILOR_API static uint32_t SerializeEngineTypes(char** yamlNode);
		SAILOR_API static uint32_t SerializeEditorTypes(char** yamlNode);
		SAILOR_API static uint32_t SerializeWorkspaceCacheIdentity(char** yamlNode);
		SAILOR_API static bool PreviewEditorAudioAsset(const char* strFileId);
		SAILOR_API static bool RequestModelFingerprint(const char* strFileId);
		SAILOR_API static uint32_t GetModelFingerprintStatus(const char* strFileId);
		SAILOR_API static bool SetEditorAnimatorParameter(
			const char* strInstanceId,
			const char* strName,
			uint32_t valueKind,
			float floatValue,
			int32_t intValue,
			bool boolValue);
		SAILOR_API static bool GetEditorAnimatorState(
			const char* strInstanceId,
			bool& outHasController,
			uint64_t& outControllerRevision,
			uint64_t& outActiveStateId,
			char** outActiveStateName,
			float& outActiveStateTime,
			bool& outTransitioning,
			uint64_t& outDestinationStateId,
			char** outDestinationStateName,
			float& outDestinationStateTime,
			float& outTransitionAlpha);
		SAILOR_API static bool ReparentEditorObject(const char* strInstanceId, const char* strParentInstanceId, bool bKeepWorldTransform);
		SAILOR_API static bool CreateEditorGameObject(const char* strParentInstanceId, const char* strPreferredInstanceId, char** outInstanceId);
		SAILOR_API static bool CreateEditorModelInstance(
			const char* strModelFileId,
			const char* strName,
			const char* strParentInstanceId,
			bool bCreateHierarchy,
			bool bHasWorldPosition,
			float worldX,
			float worldY,
			float worldZ,
			const char* strPreferredInstanceId,
			char** outInstanceId);
		SAILOR_API static bool AddEditorComponent(const char* strInstanceId, const char* strComponentTypeName, const char* strPreferredInstanceId, char** outInstanceId);
		SAILOR_API static bool InstantiateEditorPrefab(const char* strFileId, const char* strParentInstanceId);
		SAILOR_API static bool InstantiateEditorPrefabInstance(
			const char* strFileId,
			const char* strParentInstanceId,
			bool bHasWorldPosition,
			float worldX,
			float worldY,
			float worldZ,
			char** outInstanceId);
		SAILOR_API static bool InstantiateEditorPrefabFromYaml(
			const char* strPrefabYaml,
			const char* strParentInstanceId);
		SAILOR_API static bool InstantiateEditorPrefabFromYaml(
			const char* strPrefabYaml,
			const char* strParentInstanceId,
			bool bStrictInstanceIds);
		SAILOR_API static bool InstantiateEditorPrefabFromYaml(
			const char* strPrefabYaml,
			const char* strParentInstanceId,
			bool bStrictInstanceIds,
			char** outInstanceId);
		SAILOR_API static void ShowMainWindow(bool bShow);

		static SubmoduleBase* GetSubmodule(uint32_t index)
		{
			auto* instance = GetInstance();
			return instance ? instance->m_submodules[index].GetRawPtr() : nullptr;
		}

		template<typename T>
		static T* GetSubmodule()
		{
			// Header-only template statics are not ABI-stable across binary boundaries.
			// Use exported App accessors from external modules instead of GetSubmodule<T>().
			auto* instance = GetInstance();
			if (!instance)
			{
				return nullptr;
			}

			const int32_t typeId = TSubmodule<T>::GetTypeId();
			if (typeId != SubmoduleBase::InvalidSubmoduleTypeId)
			{
				return instance->m_submodules[(uint32_t)typeId].StaticCast<T>();
			}
			return nullptr;
		}

		template<typename T>
		static T* AddSubmodule(TUniquePtr<TSubmodule<T>>&& submodule)
		{
			auto* instance = GetInstance();
			check(submodule);
			check(instance);
			check(TSubmodule<T>::GetTypeId() != SubmoduleBase::InvalidSubmoduleTypeId);
			check(!instance->m_submodules[(uint32_t)TSubmodule<T>::GetTypeId()]);

			T* rawPtr = static_cast<T*>(submodule.GetRawPtr());
			instance->m_submodules[TSubmodule<T>::GetTypeId()] = std::move(submodule);

			return rawPtr;
		}

		template<typename T>
		static void RemoveSubmodule()
		{
			auto* instance = GetInstance();
			if (!instance)
			{
				return;
			}

			const int32_t typeId = TSubmodule<T>::GetTypeId();
			if (typeId == SubmoduleBase::InvalidSubmoduleTypeId)
			{
				return;
			}

			instance->m_submodules[(uint32_t)typeId].Clear();
		}

		SAILOR_API static TUniquePtr<Platform::NativeWindow>& GetMainWindow();
		SAILOR_API static Platform::Window* GetMainWindowPlatform();
		static const char* GetApplicationName() { return "SailorEngine"; }
		static const char* GetEngineName() { return "SailorEngine"; }
		SAILOR_API static const std::string& GetLoadedWorldPath();
		SAILOR_API static const char* GetBuildConfig();
		static const char* GetEngineVersion() { return "unknown"; }
		SAILOR_API static int32_t GetExitCode();
		SAILOR_API static void SetExitCode(int32_t exitCode);

	protected:

		TUniquePtr<Platform::NativeWindow> m_pMainWindow;
		TUniquePtr<Workspace::WorkspaceModuleManager> m_pWorkspaceModuleManager;
		Workspace::WorkspaceContext m_workspaceContext;
		EAppInitializationResult m_initializationResult = EAppInitializationResult::Failed;
		int32_t m_exitCode = 0;
		AppArgs m_args{};

	private:
		friend class Tests::TaskTestApp;

		SAILOR_API static bool DispatchOnEngineMainThread(std::function<void()> command);
		static void QueueAssetReloadTaskLocked(Tasks::Scheduler* scheduler);
		static void ProcessAssetReloadRequestOnEngineMainThread();

		TSharedPtr<Tasks::ITask> m_pendingAssetReloadTask;
		uint64_t m_assetReloadRequestGeneration = 0;
		uint64_t m_assetReloadCompletedGeneration = 0;
		uint64_t m_assetReloadSuccessfulGeneration = 0;
		TUniquePtr<SubmoduleBase> m_submodules[MaxSubmodules];

		static App* s_pInstance;

		App(const App&) = delete;
		App(App&&) = delete;

		App();
		~App();
	};
}
