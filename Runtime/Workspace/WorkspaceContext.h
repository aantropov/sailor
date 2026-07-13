#pragma once

#include "Core/Defines.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace Sailor::Workspace
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)
#endif

	enum class EWorkspaceContextResolveStatus : uint32_t
	{
		Success,
		Legacy,
		WorkspaceInvalid,
		ManifestNotFound,
		ManifestAmbiguous,
		ManifestInvalid,
		PathInvalid,
		ContentMissing,
		DirectoryCreationFailed
	};

	struct WorkspaceContextResolveResult;
	SAILOR_SHARED_API WorkspaceContextResolveResult ResolveWorkspaceContext(
		const std::filesystem::path& root,
		const std::filesystem::path& requestedManifest) noexcept;

	class SAILOR_SHARED_API WorkspaceContext final
	{
	public:
		const std::filesystem::path& GetRoot() const noexcept { return m_root; }
		const std::filesystem::path& GetManifest() const noexcept { return m_manifest; }
		const std::filesystem::path& GetEngineRoot() const noexcept { return m_engineRoot; }
		const std::filesystem::path& GetEngineContent() const noexcept { return m_engineContent; }
		const std::filesystem::path& GetContent() const noexcept { return m_content; }
		const std::filesystem::path& GetCache() const noexcept { return m_cache; }
		const std::filesystem::path& GetSource() const noexcept { return m_source; }
		const std::filesystem::path& GetGenerated() const noexcept { return m_generated; }
		const std::filesystem::path& GetBuild() const noexcept { return m_build; }
		const std::filesystem::path& GetLogicOutput() const noexcept { return m_logicOutput; }
		const std::string& GetModuleName() const noexcept { return m_moduleName; }
		const std::string& GetWorkspaceId() const noexcept { return m_workspaceId; }
		const std::string& GetWorkspaceName() const noexcept { return m_workspaceName; }
		uint32_t GetManifestVersion() const noexcept { return m_manifestVersion; }
		bool IsLegacy() const noexcept { return m_bLegacy; }

	private:
		std::filesystem::path m_root;
		std::filesystem::path m_manifest;
		std::filesystem::path m_engineRoot;
		std::filesystem::path m_engineContent;
		std::filesystem::path m_content;
		std::filesystem::path m_cache;
		std::filesystem::path m_source;
		std::filesystem::path m_generated;
		std::filesystem::path m_build;
		std::filesystem::path m_logicOutput;
		std::string m_moduleName;
		std::string m_workspaceId;
		std::string m_workspaceName;
		uint32_t m_manifestVersion = 0;
		bool m_bLegacy = false;

		friend struct WorkspaceContextResolveResult;
		friend WorkspaceContextResolveResult ResolveWorkspaceContext(
			const std::filesystem::path&,
			const std::filesystem::path&) noexcept;
	};

	struct SAILOR_SHARED_API WorkspaceContextResolveResult
	{
		EWorkspaceContextResolveStatus m_status = EWorkspaceContextResolveStatus::WorkspaceInvalid;
		std::string m_message;
		WorkspaceContext m_context;

		bool IsSuccess() const noexcept
		{
			return m_status == EWorkspaceContextResolveStatus::Success ||
				m_status == EWorkspaceContextResolveStatus::Legacy;
		}
	};

	SAILOR_SHARED_API WorkspaceContextResolveResult ResolveWorkspaceContext(
		const std::filesystem::path& root,
		const std::filesystem::path& requestedManifest = {}) noexcept;

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
