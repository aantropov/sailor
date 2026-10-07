#pragma once

#include "Core/Defines.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Sailor::Workspace
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)
#endif

	class WorkspaceContext;
	inline constexpr uint32_t WorkspaceCacheFormatVersion = 1;

	enum class EWorkspaceCacheLoadStatus : uint32_t
	{
		Missing,
		Loaded,
		StaleIdentity,
		Corrupt,
		UnsupportedVersion,
		IoFailure
	};

	struct SAILOR_SHARED_API WorkspaceCacheIdentity final
	{
		uint32_t m_cacheVersion = WorkspaceCacheFormatVersion;
		uint32_t m_payloadVersion = 0;
		std::string m_cacheKind;
		std::string m_producerIdentity;
		std::string m_workspaceId;
		std::string m_engineVersion;
		std::string m_buildIdentity;
	};

	struct SAILOR_SHARED_API WorkspaceCacheLoadResult final
	{
		EWorkspaceCacheLoadStatus m_status = EWorkspaceCacheLoadStatus::IoFailure;
		std::string m_diagnostic;
		std::string m_payload;

		bool IsLoaded() const noexcept
		{
			return m_status == EWorkspaceCacheLoadStatus::Loaded;
		}
	};

	SAILOR_SHARED_API std::string ResolveWorkspaceCacheIdentity(
		std::string_view workspaceId,
		const std::filesystem::path& canonicalWorkspaceRoot);

	SAILOR_SHARED_API const std::string& GetWorkspaceCacheEngineVersion();
	SAILOR_SHARED_API const std::string& GetWorkspaceCacheBuildIdentity();

	SAILOR_SHARED_API WorkspaceCacheIdentity MakeWorkspaceCacheIdentity(
		std::string_view cacheKind,
		std::string_view producerIdentity,
		uint32_t payloadVersion,
		const WorkspaceContext& workspaceContext);

	SAILOR_SHARED_API WorkspaceCacheIdentity MakeWorkspaceCacheIdentity(
		std::string_view cacheKind,
		std::string_view producerIdentity,
		uint32_t payloadVersion,
		std::string_view workspaceId,
		const std::filesystem::path& canonicalWorkspaceRoot);

	SAILOR_SHARED_API bool SerializeWorkspaceCacheEnvelope(
		const WorkspaceCacheIdentity& identity,
		std::string_view payload,
		std::string& outEnvelope,
		std::string& outDiagnostic) noexcept;

	SAILOR_SHARED_API WorkspaceCacheLoadResult ParseWorkspaceCacheEnvelope(
		const std::string& envelope,
		const WorkspaceCacheIdentity& expectedIdentity,
		std::string_view sourceName = "workspace cache") noexcept;

	SAILOR_SHARED_API WorkspaceCacheLoadResult LoadWorkspaceCacheEnvelope(
		const std::filesystem::path& path,
		const WorkspaceCacheIdentity& expectedIdentity) noexcept;

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
