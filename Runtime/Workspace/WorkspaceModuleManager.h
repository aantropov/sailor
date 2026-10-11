#pragma once

#include "Core/Defines.h"
#include "Memory/UniquePtr.hpp"
#include "Platform/DynamicLibrary.h"
#include "Workspace/WorkspaceContext.h"

#include <filesystem>
#include <string>

namespace YAML
{
	class Node;
}

namespace Sailor
{
	struct ReflectedTypeCatalog;
}

namespace Sailor::Workspace
{
	enum class EWorkspaceModuleState : uint32_t
	{
		NotConfigured,
		Loaded,
		Registered,
		Failed,
		Unloaded
	};

	enum class EWorkspaceModuleLoadStatus : uint32_t
	{
		NotConfigured,
		Success,
		WorkspaceInvalid,
		ManifestNotFound,
		ManifestAmbiguous,
		ManifestInvalid,
		ModuleNotFound,
		NativeLoadFailed,
		EntryPointMissing,
		ApiInvalid,
		AbiMismatch,
		MetadataInvalid,
		RegistrationFailed,
		NativeUnloadFailed
	};

	struct WorkspaceModuleLoadResult
	{
		EWorkspaceModuleLoadStatus m_status = EWorkspaceModuleLoadStatus::NotConfigured;
		std::string m_message;
		std::filesystem::path m_manifestPath;
		std::filesystem::path m_modulePath;
		std::string m_moduleName;
		std::string m_buildConfig;
		size_t m_numRegisteredTypes = 0;

		bool IsSuccess() const noexcept { return m_status == EWorkspaceModuleLoadStatus::Success; }
		bool IsFailure() const noexcept
		{
			return m_status != EWorkspaceModuleLoadStatus::NotConfigured && !IsSuccess();
		}
	};

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)
#endif

	class SAILOR_SHARED_API WorkspaceModuleManager final
	{
	public:
		WorkspaceModuleManager() noexcept;
		~WorkspaceModuleManager() noexcept;

		WorkspaceModuleManager(const WorkspaceModuleManager&) = delete;
		WorkspaceModuleManager& operator=(const WorkspaceModuleManager&) = delete;
		WorkspaceModuleManager(WorkspaceModuleManager&&) = delete;
		WorkspaceModuleManager& operator=(WorkspaceModuleManager&&) = delete;

		const WorkspaceModuleLoadResult& Load(
			const WorkspaceContext& context,
			std::string buildConfig) noexcept;
		bool Unload() noexcept;
		bool BuildEditorTypeMetadata(
			const YAML::Node& engineMetadata,
			YAML::Node& outMetadata,
			std::string& outError) const noexcept;

		EWorkspaceModuleState GetState() const noexcept { return m_state; }
		const WorkspaceModuleLoadResult& GetResult() const noexcept { return m_result; }
		const std::string& GetMetadata() const noexcept { return m_metadata; }
		uint64_t GetTypeCatalogHash() const noexcept { return m_typeCatalogHash; }
		bool IsRegistered() const noexcept { return m_state == EWorkspaceModuleState::Registered; }

	private:
		const WorkspaceModuleLoadResult& Fail(EWorkspaceModuleLoadStatus status, std::string message) noexcept;

		Platform::DynamicLibrary m_library;
		EWorkspaceModuleState m_state = EWorkspaceModuleState::NotConfigured;
		WorkspaceModuleLoadResult m_result;
		std::string m_owner;
		std::string m_metadata;
		uint64_t m_typeCatalogHash = 0;
		TUniquePtr<ReflectedTypeCatalog> m_editorMetadata;
	};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
