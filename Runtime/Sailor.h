#pragma once
#include "Core/Defines.h"
#include "Core/LogMacros.h"
#include "Core/Utils.h"
#include "Core/Submodule.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "Memory/UniquePtr.hpp"
#include "Platform/Win32/Window.h"
#include "Containers/Containers.h"
#include "Math/Math.h"

namespace Sailor
{
	struct AppArgs
	{
		bool m_bWaitForDebugger = false;
		bool m_bRunConsole = true;
		bool m_bIsEditor = false;
		bool m_bEnableRenderValidationLayers = true;
		bool m_bRunPathTracer = false;

		uint32_t m_editorPort = 32800;
		HWND m_editorHwnd{};
		std::string m_workspace;

		// TODO: Change the default scene to empty?
		std::string m_world = "Editor.world";
	};

	class App
	{
		static constexpr size_t MaxSubmodules = 128u;
		static std::string s_workspace;

	public:

		SAILOR_API static App* GetInstance();
		SAILOR_API static const std::string& GetWorkspace();

		SAILOR_API static void Initialize(const char** commandLineArgs = nullptr, int32_t num = 0);
		SAILOR_API static void Start();
		SAILOR_API static void Stop();
		SAILOR_API static void Shutdown();
		SAILOR_API static bool IsRendererInitialized();
		SAILOR_API static bool HasEditor();
		SAILOR_API static bool IsEditorMode();
		SAILOR_API static void SetEditorViewport(uint32_t windowPosX, uint32_t windowPosY, uint32_t width, uint32_t height);
		SAILOR_API static void SetEditorRenderTargetSize(uint32_t width, uint32_t height);
		SAILOR_API static bool UpsertEditorRemoteViewport(uint64_t viewportId, uint32_t windowPosX, uint32_t windowPosY, uint32_t width, uint32_t height, bool bVisible, bool bFocused);
		SAILOR_API static bool DestroyEditorRemoteViewport(uint64_t viewportId);
		SAILOR_API static uint32_t GetEditorRemoteViewportState(uint64_t viewportId);
		SAILOR_API static uint32_t GetEditorRemoteViewportDiagnostics(uint64_t viewportId, char** diagnostics);
		SAILOR_API static bool RetryEditorRemoteViewport(uint64_t viewportId);
		SAILOR_API static bool SetEditorRemoteViewportMacHostHandle(uint64_t viewportId, uint32_t hostHandleKind, uint64_t hostHandleValue);
		SAILOR_API static bool SendEditorRemoteViewportInput(uint64_t viewportId, uint32_t kind, float pointerX, float pointerY, float wheelDeltaX, float wheelDeltaY, uint32_t keyCode, uint32_t button, uint32_t modifiers, bool bPressed, bool bFocused, bool bCaptured);
		SAILOR_API static uint32_t PullEditorMessages(char** messages, uint32_t num);
		SAILOR_API static uint32_t SerializeCurrentWorld(char** yamlNode);
		SAILOR_API static uint32_t SerializeEngineTypes(char** yamlNode);
		SAILOR_API static bool UpdateEditorObject(const char* strInstanceId, const char* strYamlNode);
		SAILOR_API static bool ReparentEditorObject(const char* strInstanceId, const char* strParentInstanceId, bool bKeepWorldTransform);
		SAILOR_API static bool InstantiateEditorPrefab(const char* strFileId, const char* strParentInstanceId);
		SAILOR_API static bool RenderPathTracedImage(const char* strOutputPath, const char* strInstanceId, uint32_t height, uint32_t samplesPerPixel, uint32_t maxBounces);
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

		SAILOR_API static TUniquePtr<Win32::Window>& GetMainWindow();
		SAILOR_API static Platform::Window* GetMainWindowPlatform();
		static const char* GetApplicationName() { return "SailorEngine"; }
		static const char* GetEngineName() { return "SailorEngine"; }
		SAILOR_API static const std::string& GetLoadedWorldPath();
		SAILOR_API static const char* GetBuildConfig();
		static const char* GetEngineVersion() { return "unknown"; }
		SAILOR_API static int32_t GetExitCode();
		SAILOR_API static void SetExitCode(int32_t exitCode);

	protected:

		TUniquePtr<Win32::Window> m_pMainWindow;
		bool m_bSkipMainLoop = false;
		int32_t m_exitCode = 0;
		AppArgs m_args{};

	private:

		TUniquePtr<SubmoduleBase> m_submodules[MaxSubmodules];

		static App* s_pInstance;

		App(const App&) = delete;
		App(App&&) = delete;

		App() = default;
		~App() = default;
	};
}
