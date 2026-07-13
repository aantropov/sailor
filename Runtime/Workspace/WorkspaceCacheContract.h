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

	enum class EWorkspaceCacheAtomicWriteFailurePoint : uint32_t
	{
		None,
		BeforeReplace
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
		const std::string& workspaceId,
		const std::filesystem::path& canonicalWorkspaceRoot);

	SAILOR_SHARED_API const std::string& GetWorkspaceCacheEngineVersion();
	SAILOR_SHARED_API const std::string& GetWorkspaceCacheBuildIdentity();

	SAILOR_SHARED_API WorkspaceCacheIdentity MakeWorkspaceCacheIdentity(
		const std::string& cacheKind,
		const std::string& producerIdentity,
		uint32_t payloadVersion,
		const WorkspaceContext& workspaceContext);

	SAILOR_SHARED_API WorkspaceCacheIdentity MakeWorkspaceCacheIdentity(
		const std::string& cacheKind,
		const std::string& producerIdentity,
		uint32_t payloadVersion,
		const std::string& workspaceId,
		const std::filesystem::path& canonicalWorkspaceRoot);

	SAILOR_SHARED_API bool SerializeWorkspaceCacheEnvelope(
		const WorkspaceCacheIdentity& identity,
		const std::string& payload,
		std::string& outEnvelope,
		std::string& outDiagnostic) noexcept;

	SAILOR_SHARED_API WorkspaceCacheLoadResult ParseWorkspaceCacheEnvelope(
		const std::string& envelope,
		const WorkspaceCacheIdentity& expectedIdentity,
		const std::string& sourceName = "workspace cache") noexcept;

	SAILOR_SHARED_API WorkspaceCacheLoadResult LoadWorkspaceCacheEnvelope(
		const std::filesystem::path& path,
		const WorkspaceCacheIdentity& expectedIdentity) noexcept;

	SAILOR_SHARED_API bool AtomicReplaceWorkspaceCacheBinary(
		const std::filesystem::path& target,
		const void* data,
		uint64_t size,
		std::string& outDiagnostic,
		EWorkspaceCacheAtomicWriteFailurePoint failurePoint =
			EWorkspaceCacheAtomicWriteFailurePoint::None) noexcept;

	SAILOR_SHARED_API bool AtomicReplaceWorkspaceCacheText(
		const std::filesystem::path& target,
		const std::string& text,
		std::string& outDiagnostic,
		EWorkspaceCacheAtomicWriteFailurePoint failurePoint =
			EWorkspaceCacheAtomicWriteFailurePoint::None) noexcept;

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
