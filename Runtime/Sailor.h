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
		static void Start(const char** commandLineArgs = nullptr, int32_t num = 0);
		static void Stop();
		static void Shutdown();

		static TUniquePtr<Win32::Window>& GetViewportWindow();

		static SubmoduleBase* GetSubmodule(uint32_t index)
		{
			return s_pInstance->m_submodules[index].GetRawPtr();
		}

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