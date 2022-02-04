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
#include <glm/glm/glm.hpp>

namespace Sailor
{
	class AssetRegistry;

	class SAILOR_API App
	{

	public:

		static constexpr size_t MaxSubmodules = 64u;
		static const char* ApplicationName;
		static const char* EngineName;

		static void Initialize();
		static void Start();
		static void Stop();
		static void Shutdown();

		static TUniquePtr<Win32::Window>& GetViewportWindow();

		template<typename T>
		static T* GetSubmodule()
		{
			const int32_t typeId = TSubmodule<T>::GetTypeId();
			if (typeId != SubmoduleBase::InvalidSubmoduleTypeId)
			{
				return static_cast<T*>(s_pInstance->m_submodules[(uint32_t)typeId].GetRawPtr());
			}
			return nullptr;
		}

		template<typename T>
		static T* AddSubmodule(TUniquePtr<TSubmodule<T>>&& submodule)
		{
			assert(submodule);
			assert(TSubmodule<T>::GetTypeId() != SubmoduleBase::InvalidSubmoduleTypeId);
			assert(!s_pInstance->m_submodules[(uint32_t)TSubmodule<T>::GetTypeId()]);

			T* rawPtr = static_cast<T*>(submodule.GetRawPtr());
			s_pInstance->m_submodules[TSubmodule<T>::GetTypeId()] = std::move(submodule);

			return rawPtr;
		}

		template<typename T>
		static void RemoveSubmodule()
		{
			assert(TSubmodule<T>::GetTypeId() != SubmoduleBase::InvalidSubmoduleTypeId);
			s_pInstance->m_submodules[TSubmodule<T>::GetTypeId()].Clear();
		}

	protected:

		TUniquePtr<Win32::Window> m_pViewportWindow;

	private:

		TUniquePtr<SubmoduleBase> m_submodules[MaxSubmodules];

		static App* s_pInstance;

		App(const App&) = delete;
		App(App&&) = delete;

		App() = default;
		~App() = default;
	};
}