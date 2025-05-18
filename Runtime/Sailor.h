#pragma once
#include "Core/Defines.h"
#include "Core/LogMacros.h"
#include "Core/Utils.h"
#include "Core/Submodule.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "Memory/UniquePtr.hpp"
#include "Platform/Window.h"
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

		static const std::string& GetWorkspace() { return s_workspace; }

		SAILOR_API static void Initialize(const char** commandLineArgs = nullptr, int32_t num = 0);
		SAILOR_API static void Start();
		SAILOR_API static void Stop();
		SAILOR_API static void Shutdown();

		static SubmoduleBase* GetSubmodule(uint32_t index) { return s_pInstance->m_submodules[index].GetRawPtr(); }

		template<typename T>
		static T* GetSubmodule()
		{
			const int32_t typeId = TSubmodule<T>::GetTypeId();
			if (typeId != SubmoduleBase::InvalidSubmoduleTypeId)
			{
				return s_pInstance->m_submodules[(uint32_t)typeId].StaticCast<T>();
			}
			return nullptr;
		}

		template<typename T>
		static T* AddSubmodule(TUniquePtr<TSubmodule<T>>&& submodule)
		{
			check(submodule);
			check(TSubmodule<T>::GetTypeId() != SubmoduleBase::InvalidSubmoduleTypeId);
			check(!s_pInstance->m_submodules[(uint32_t)TSubmodule<T>::GetTypeId()]);

			T* rawPtr = static_cast<T*>(submodule.GetRawPtr());
			s_pInstance->m_submodules[TSubmodule<T>::GetTypeId()] = std::move(submodule);

			return rawPtr;
		}

		template<typename T>
		static void RemoveSubmodule()
		{
			check(TSubmodule<T>::GetTypeId() != SubmoduleBase::InvalidSubmoduleTypeId);
			s_pInstance->m_submodules[TSubmodule<T>::GetTypeId()].Clear();
		}

               static TUniquePtr<Platform::Window>& GetMainWindow();
		static const char* GetApplicationName() { return "SailorEngine"; }
		static const char* GetEngineName() { return "SailorEngine"; }

	protected:

               TUniquePtr<Platform::Window> m_pMainWindow;

	private:

		TUniquePtr<SubmoduleBase> m_submodules[MaxSubmodules];

		static App* s_pInstance;

		App(const App&) = delete;
		App(App&&) = delete;

		App() = default;
		~App() = default;
	};
}