#pragma once
#include "Core/Defines.h"
#include "Core/LogMacros.h"
#include "Core/Utils.h"
#include "Core/Submodule.hpp"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "Memory/UniquePtr.hpp"
#include "Platform/Win32/Window.h"
#include <glm/glm/glm.hpp>

namespace Sailor
{
	class AssetRegistry;

	class SAILOR_API EngineInstance
	{

	public:

		static const char* ApplicationName;
		static const char* EngineName;

		static void Initialize();
		static void Start();
		static void Stop();
		static void Shutdown();

		static TUniquePtr<Win32::Window>& GetViewportWindow();

		template<typename TSubmodule>
		static TSubmodule* GetSubmodule() { return nullptr; }

		template<>
		static AssetRegistry* GetSubmodule<AssetRegistry>() { return m_pInstance->m_pAssetRegistry; }

	protected:

		TUniquePtr<Win32::Window> m_pViewportWindow;

	private:

		AssetRegistry* m_pAssetRegistry = nullptr;
		static EngineInstance* m_pInstance;

		EngineInstance(const EngineInstance&) = delete;
		EngineInstance(EngineInstance&&) = delete;

		EngineInstance() = default;
		~EngineInstance() = default;
	};
}