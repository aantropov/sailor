#include "Workspace/WorkspaceCacheContract.h"
#include "Containers/Containers.h"

#include "Workspace/WorkspaceContext.h"
#include "Workspace/WorkspaceModuleApi.h"
#include "YamlExceptionBoundary.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <yaml-cpp/yaml.h>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#if !defined(SAILOR_ENGINE_VERSION)
#define SAILOR_ENGINE_VERSION "unknown"
#endif

#if !defined(SAILOR_BUILD_CONFIG)
#define SAILOR_BUILD_CONFIG "unknown"
#endif

namespace
{
	using Sailor::TSet;
	using namespace Sailor::Workspace;

	constexpr const char* CacheVersionField = "cacheVersion";
	constexpr const char* PayloadVersionField = "payloadVersion";
	constexpr const char* CacheKindField = "cacheKind";
	constexpr const char* ProducerIdentityField = "producerIdentity";
	constexpr const char* WorkspaceIdField = "workspaceId";
	constexpr const char* EngineVersionField = "engineVersion";
	constexpr const char* BuildIdentityField = "buildIdentity";
	constexpr const char* PayloadField = "payload";

	const TSet<std::string> EnvelopeFields =
	{
		CacheVersionField,
		PayloadVersionField,
		CacheKindField,
		ProducerIdentityField,
		WorkspaceIdField,
		EngineVersionField,
		BuildIdentityField,
		PayloadField
	};

	std::atomic<uint64_t> TemporaryFileCounter = 0;

	WorkspaceCacheLoadResult Fail(
		EWorkspaceCacheLoadStatus status,
		std::string diagnostic) noexcept
	{
		WorkspaceCacheLoadResult result;
		result.m_status = status;
		result.m_diagnostic = std::move(diagnostic);
		return result;
	}

	std::string Quote(const std::string& value)
	{
		return "'" + value + "'";
	}

	std::string SourceLabel(const std::string& sourceName)
	{
		return sourceName.empty() ? "workspace cache" : sourceName;
	}

	YAML::Node FindField(const YAML::Node& document, const char* fieldName)
	{
		for (const auto& field : document)
		{
			if (field.first.IsScalar() && field.first.Scalar() == fieldName)
			{
				return field.second;
			}
		}

		return YAML::Node(YAML::NodeType::Undefined);
	}

	bool ValidateMapKeys(
		const YAML::Node& document,
		const std::string& sourceName,
		std::string& outDiagnostic)
	{
		TSet<std::string> keys;
		for (const auto& field : document)
		{
			if (!field.first.IsScalar())
			{
				outDiagnostic = sourceName + " is corrupt: its envelope contains a non-scalar field name.";
				return false;
			}

			const std::string key = field.first.Scalar();
			if (key.empty() || !keys.Insert(key))
			{
				outDiagnostic = sourceName + " is corrupt: its envelope contains duplicate or empty field " +
					Quote(key) + ".";
				return false;
			}
		}

		return true;
	}

	bool ValidateCurrentEnvelopeFields(
		const YAML::Node& document,
		const std::string& sourceName,
		std::string& outDiagnostic)
	{
		for (const auto& field : document)
		{
			const std::string key = field.first.Scalar();
			if (!EnvelopeFields.Contains(key))
			{
				outDiagnostic = sourceName + " is corrupt: its current envelope contains unknown field " +
					Quote(key) + ".";
				return false;
			}
		}

		for (const std::string& requiredField : EnvelopeFields)
		{
			if (!FindField(document, requiredField.c_str()).IsDefined())
			{
				outDiagnostic = sourceName + " is corrupt: its current envelope is missing required field " +
					Quote(requiredField) + ".";
				return false;
			}
		}

		return true;
	}

	bool ReadUint32(
		const YAML::Node& field,
		const char* fieldName,
		const std::string& sourceName,
		uint32_t& outValue,
		std::string& outDiagnostic)
	{
		if (!field.IsDefined() || !field.IsScalar())
		{
			outDiagnostic = sourceName + " is corrupt: field " + Quote(fieldName) +
				" must be an unsigned integer scalar.";
			return false;
		}

		const std::string scalar = field.Scalar();
		if (scalar.empty() ||
			!std::all_of(scalar.begin(), scalar.end(), [](unsigned char character)
				{
					return std::isdigit(character) != 0;
				}))
		{
			outDiagnostic = sourceName + " is corrupt: field " + Quote(fieldName) +
				" must be an unsigned integer scalar.";
			return false;
		}

		const char* begin = scalar.data();
		const char* end = begin + scalar.size();
		const auto parsed = std::from_chars(begin, end, outValue);
		if (parsed.ec != std::errc() || parsed.ptr != end)
		{
			outDiagnostic = sourceName + " is corrupt: field " + Quote(fieldName) +
				" must be an unsigned 32-bit integer scalar.";
			return false;
		}
		return true;
	}

	bool ReadRequiredString(
		const YAML::Node& document,
		const char* fieldName,
		const std::string& sourceName,
		std::string& outValue,
		std::string& outDiagnostic,
		bool bAllowEmpty = false)
	{
		const YAML::Node field = FindField(document, fieldName);
		if (!field.IsDefined() || !field.IsScalar())
		{
			outDiagnostic = sourceName + " is corrupt: field " + Quote(fieldName) +
				" must be a scalar.";
			return false;
		}

		outValue = field.Scalar();
		if (!bAllowEmpty && outValue.empty())
		{
			outDiagnostic = sourceName + " is corrupt: field " + Quote(fieldName) +
				" cannot be empty.";
			return false;
		}
		return true;
	}

	WorkspaceCacheLoadResult VersionMismatch(
		const std::string& sourceName,
		const char* fieldName,
		uint32_t expected,
		const std::string& actual)
	{
		return Fail(
			EWorkspaceCacheLoadStatus::UnsupportedVersion,
			sourceName + " has unsupported " + fieldName +
			" (expected " + Quote(std::to_string(expected)) +
			", actual " + Quote(actual) + ").");
	}

	WorkspaceCacheLoadResult IdentityMismatch(
		const std::string& sourceName,
		const char* fieldName,
		const std::string& expected,
		const std::string& actual)
	{
		return Fail(
			EWorkspaceCacheLoadStatus::StaleIdentity,
			sourceName + " has stale identity field " + Quote(fieldName) +
			" (expected " + Quote(expected) + ", actual " + Quote(actual) + ").");
	}

	bool ValidateIdentityForSerialization(
		const WorkspaceCacheIdentity& identity,
		std::string& outDiagnostic)
	{
		if (identity.m_cacheVersion != WorkspaceCacheFormatVersion)
		{
			outDiagnostic = "Cannot serialize workspace cache envelope: cacheVersion must be " +
				Quote(std::to_string(WorkspaceCacheFormatVersion)) + ", actual " +
				Quote(std::to_string(identity.m_cacheVersion)) + ".";
			return false;
		}
		if (identity.m_payloadVersion == 0)
		{
			outDiagnostic = "Cannot serialize workspace cache envelope: payloadVersion must be greater than zero.";
			return false;
		}

		const std::pair<const char*, const std::string*> fields[] =
		{
			{ CacheKindField, &identity.m_cacheKind },
			{ ProducerIdentityField, &identity.m_producerIdentity },
			{ WorkspaceIdField, &identity.m_workspaceId },
			{ EngineVersionField, &identity.m_engineVersion },
			{ BuildIdentityField, &identity.m_buildIdentity }
		};
		for (const auto& [fieldName, value] : fields)
		{
			if (value->empty())
			{
				outDiagnostic = "Cannot serialize workspace cache envelope: field " +
					Quote(fieldName) + " cannot be empty.";
				return false;
			}
		}

		return true;
	}

	std::filesystem::path NormalizeLegacyRoot(const std::filesystem::path& root)
	{
		std::error_code error;
		std::filesystem::path normalized = std::filesystem::weakly_canonical(root, error);
		if (error)
		{
			error.clear();
			normalized = std::filesystem::absolute(root, error);
			if (error)
			{
				normalized = root;
			}
		}
		return normalized.lexically_normal();
	}

	std::string GenericUtf8String(const std::filesystem::path& path)
	{
		const std::u8string utf8 = path.generic_u8string();
		return std::string(
			reinterpret_cast<const char*>(utf8.data()),
			utf8.size());
	}

#if defined(_WIN32)
	void FoldAsciiCaseForWindows(std::string& value) noexcept
	{
		for (char& character : value)
		{
			if (character >= 'A' && character <= 'Z')
			{
				character = static_cast<char>(character + ('a' - 'A'));
			}
		}
	}
#endif

	uint64_t GetProcessIdentity() noexcept
	{
#if defined(_WIN32)
		return static_cast<uint64_t>(GetCurrentProcessId());
#else
		return static_cast<uint64_t>(getpid());
#endif
	}

	std::filesystem::path MakeTemporaryPath(const std::filesystem::path& target)
	{
		const uint64_t counter = TemporaryFileCounter.fetch_add(1, std::memory_order_relaxed) + 1;
		const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
		std::filesystem::path filename = target.filename();
		filename += "." + std::to_string(GetProcessIdentity()) + "." +
			std::to_string(timestamp) + "." +
			std::to_string(counter) + ".tmp";
		return target.parent_path() / filename;
	}

	class TemporaryFileCleanup final
	{
	public:
		explicit TemporaryFileCleanup(std::filesystem::path path) : m_path(std::move(path)) {}

		~TemporaryFileCleanup() noexcept
		{
			if (!m_bReleased)
			{
				std::error_code error;
				std::filesystem::remove(m_path, error);
			}
		}

		void Release() noexcept { m_bReleased = true; }

	private:
		std::filesystem::path m_path;
		bool m_bReleased = false;
	};

#if defined(_WIN32)
	std::string WindowsErrorMessage(DWORD error)
	{
		return std::system_category().message(static_cast<int>(error));
	}

	bool WriteTemporaryFile(
		const std::filesystem::path& temporaryPath,
		const void* data,
		uint64_t size,
		std::string& outDiagnostic)
	{
		HANDLE file = CreateFileW(
			temporaryPath.c_str(),
			GENERIC_WRITE,
			0,
			nullptr,
			CREATE_NEW,
			FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH,
			nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			const DWORD error = GetLastError();
			outDiagnostic = "Cannot create workspace cache temporary file " +
				Quote(temporaryPath.generic_string()) + ": " + WindowsErrorMessage(error) + ".";
			return false;
		}

		const uint8_t* cursor = static_cast<const uint8_t*>(data);
		uint64_t remaining = size;
		while (remaining > 0)
		{
			const DWORD chunk = static_cast<DWORD>((std::min)(
				remaining,
				static_cast<uint64_t>((std::numeric_limits<DWORD>::max)())));
			DWORD written = 0;
			if (!WriteFile(file, cursor, chunk, &written, nullptr) || written != chunk)
			{
				const DWORD error = GetLastError();
				CloseHandle(file);
				outDiagnostic = "Cannot write workspace cache temporary file " +
					Quote(temporaryPath.generic_string()) + ": " + WindowsErrorMessage(error) + ".";
				return false;
			}
			cursor += written;
			remaining -= written;
		}

		if (!FlushFileBuffers(file))
		{
			const DWORD error = GetLastError();
			CloseHandle(file);
			outDiagnostic = "Cannot flush workspace cache temporary file " +
				Quote(temporaryPath.generic_string()) + ": " + WindowsErrorMessage(error) + ".";
			return false;
		}

		if (!CloseHandle(file))
		{
			const DWORD error = GetLastError();
			outDiagnostic = "Cannot close workspace cache temporary file " +
				Quote(temporaryPath.generic_string()) + ": " + WindowsErrorMessage(error) + ".";
			return false;
		}

		return true;
	}
#else
	std::string PosixErrorMessage(int error)
	{
		return std::generic_category().message(error);
	}

	bool WriteTemporaryFile(
		const std::filesystem::path& temporaryPath,
		const void* data,
		uint64_t size,
		std::string& outDiagnostic)
	{
		int flags = O_WRONLY | O_CREAT | O_EXCL;
#if defined(O_CLOEXEC)
		flags |= O_CLOEXEC;
#endif
		const int file = open(temporaryPath.c_str(), flags, 0666);
		if (file < 0)
		{
			const int error = errno;
			outDiagnostic = "Cannot create workspace cache temporary file " +
				Quote(temporaryPath.generic_string()) + ": " + PosixErrorMessage(error) + ".";
			return false;
		}

		const uint8_t* cursor = static_cast<const uint8_t*>(data);
		uint64_t remaining = size;
		while (remaining > 0)
		{
			const size_t chunk = static_cast<size_t>((std::min)(
				remaining,
				static_cast<uint64_t>((std::numeric_limits<ssize_t>::max)())));
			const ssize_t written = write(file, cursor, chunk);
			if (written < 0)
			{
				if (errno == EINTR)
				{
					continue;
				}

				const int error = errno;
				close(file);
				outDiagnostic = "Cannot write workspace cache temporary file " +
					Quote(temporaryPath.generic_string()) + ": " + PosixErrorMessage(error) + ".";
				return false;
			}
			if (written == 0)
			{
				close(file);
				outDiagnostic = "Cannot write workspace cache temporary file " +
					Quote(temporaryPath.generic_string()) + ": the write made no progress.";
				return false;
			}

			cursor += written;
			remaining -= static_cast<uint64_t>(written);
		}

		if (fsync(file) != 0)
		{
			const int error = errno;
			close(file);
			outDiagnostic = "Cannot flush workspace cache temporary file " +
				Quote(temporaryPath.generic_string()) + ": " + PosixErrorMessage(error) + ".";
			return false;
		}

		if (close(file) != 0)
		{
			const int error = errno;
			outDiagnostic = "Cannot close workspace cache temporary file " +
				Quote(temporaryPath.generic_string()) + ": " + PosixErrorMessage(error) + ".";
			return false;
		}

		return true;
	}

	bool IsUnsupportedDirectorySyncError(int error) noexcept
	{
		return error == EINVAL
#if defined(ENOTSUP)
			|| error == ENOTSUP
#endif
#if defined(EOPNOTSUPP) && (!defined(ENOTSUP) || EOPNOTSUPP != ENOTSUP)
			|| error == EOPNOTSUPP
#endif
			;
	}

	bool FlushDirectory(
		const std::filesystem::path& directory,
		std::string& outDiagnostic)
	{
		int flags = O_RDONLY;
#if defined(O_CLOEXEC)
		flags |= O_CLOEXEC;
#endif
#if defined(O_DIRECTORY)
		flags |= O_DIRECTORY;
#endif
		const int directoryFile = open(directory.c_str(), flags);
		if (directoryFile < 0)
		{
			const int error = errno;
			outDiagnostic = "Workspace cache was replaced, but its directory could not be opened for durability sync " +
				Quote(directory.generic_string()) + ": " + PosixErrorMessage(error) + ".";
			return false;
		}

		if (fsync(directoryFile) != 0)
		{
			const int error = errno;
			close(directoryFile);
			if (IsUnsupportedDirectorySyncError(error))
			{
				return true;
			}
			outDiagnostic = "Workspace cache was replaced, but its directory durability sync failed " +
				Quote(directory.generic_string()) + ": " + PosixErrorMessage(error) + ".";
			return false;
		}

		if (close(directoryFile) != 0)
		{
			const int error = errno;
			outDiagnostic = "Workspace cache was replaced, but its directory handle could not be closed " +
				Quote(directory.generic_string()) + ": " + PosixErrorMessage(error) + ".";
			return false;
		}

		return true;
	}
#endif
}

using namespace Sailor::Workspace;

std::string Sailor::Workspace::ResolveWorkspaceCacheIdentity(
	const std::string& workspaceId,
	const std::filesystem::path& canonicalWorkspaceRoot)
{
	if (!workspaceId.empty())
	{
		return workspaceId;
	}

	std::string normalizedRoot = GenericUtf8String(NormalizeLegacyRoot(canonicalWorkspaceRoot));
#if defined(_WIN32)
	FoldAsciiCaseForWindows(normalizedRoot);
#endif
	return "legacy-root:" + normalizedRoot;
}

const std::string& Sailor::Workspace::GetWorkspaceCacheEngineVersion()
{
	static const std::string EngineVersion = SAILOR_ENGINE_VERSION;
	return EngineVersion;
}

const std::string& Sailor::Workspace::GetWorkspaceCacheBuildIdentity()
{
	static const std::string BuildIdentity =
		std::string("config=") + SAILOR_BUILD_CONFIG + ";" + GetWorkspaceModuleAbiTagV1();
	return BuildIdentity;
}

WorkspaceCacheIdentity Sailor::Workspace::MakeWorkspaceCacheIdentity(
	const std::string& cacheKind,
	const std::string& producerIdentity,
	uint32_t payloadVersion,
	const WorkspaceContext& workspaceContext)
{
	return MakeWorkspaceCacheIdentity(
		cacheKind,
		producerIdentity,
		payloadVersion,
		workspaceContext.GetWorkspaceId(),
		workspaceContext.GetRoot());
}

WorkspaceCacheIdentity Sailor::Workspace::MakeWorkspaceCacheIdentity(
	const std::string& cacheKind,
	const std::string& producerIdentity,
	uint32_t payloadVersion,
	const std::string& workspaceId,
	const std::filesystem::path& canonicalWorkspaceRoot)
{
	WorkspaceCacheIdentity identity;
	identity.m_cacheVersion = WorkspaceCacheFormatVersion;
	identity.m_payloadVersion = payloadVersion;
	identity.m_cacheKind = cacheKind;
	identity.m_producerIdentity = producerIdentity;
	identity.m_workspaceId = ResolveWorkspaceCacheIdentity(workspaceId, canonicalWorkspaceRoot);
	identity.m_engineVersion = GetWorkspaceCacheEngineVersion();
	identity.m_buildIdentity = GetWorkspaceCacheBuildIdentity();
	return identity;
}

bool Sailor::Workspace::SerializeWorkspaceCacheEnvelope(
	const WorkspaceCacheIdentity& identity,
	const std::string& payload,
	std::string& outEnvelope,
	std::string& outDiagnostic) noexcept
{
	outEnvelope.clear();
	outDiagnostic.clear();
	if (!ValidateIdentityForSerialization(identity, outDiagnostic))
	{
		return false;
	}

	YAML::Node document;
	document[CacheVersionField] = identity.m_cacheVersion;
	document[PayloadVersionField] = identity.m_payloadVersion;
	document[CacheKindField] = identity.m_cacheKind;
	document[ProducerIdentityField] = identity.m_producerIdentity;
	document[WorkspaceIdField] = identity.m_workspaceId;
	document[EngineVersionField] = identity.m_engineVersion;
	document[BuildIdentityField] = identity.m_buildIdentity;
	document[PayloadField] = payload;
	std::string yamlDiagnostic;
	if (!Sailor::External::TryDumpYaml(document, outEnvelope, yamlDiagnostic))
	{
		outEnvelope.clear();
		outDiagnostic = "Cannot serialize workspace cache envelope: " + yamlDiagnostic;
		return false;
	}
	return true;
}

WorkspaceCacheLoadResult Sailor::Workspace::ParseWorkspaceCacheEnvelope(
	const std::string& envelope,
	const WorkspaceCacheIdentity& expectedIdentity,
	const std::string& sourceName) noexcept
{
	const std::string source = SourceLabel(sourceName);
	YAML::Node document;
	std::string yamlDiagnostic;
	if (!Sailor::External::TryLoadYaml(envelope, document, yamlDiagnostic))
	{
		return Fail(
			EWorkspaceCacheLoadStatus::Corrupt,
			source + " is corrupt: invalid YAML: " + yamlDiagnostic);
	}
	if (!document.IsMap())
	{
		return Fail(
			EWorkspaceCacheLoadStatus::Corrupt,
			source + " is corrupt: its envelope must be a YAML map.");
	}

	std::string diagnostic;
	if (!ValidateMapKeys(document, source, diagnostic))
	{
		return Fail(EWorkspaceCacheLoadStatus::Corrupt, std::move(diagnostic));
	}

	const YAML::Node cacheVersionField = FindField(document, CacheVersionField);
	if (!cacheVersionField.IsDefined())
	{
		return VersionMismatch(
			source,
			CacheVersionField,
			WorkspaceCacheFormatVersion,
			"missing");
	}

	uint32_t cacheVersion = 0;
	if (!ReadUint32(
			cacheVersionField,
			CacheVersionField,
			source,
			cacheVersion,
			diagnostic))
	{
		return Fail(EWorkspaceCacheLoadStatus::Corrupt, std::move(diagnostic));
	}
	if (cacheVersion != WorkspaceCacheFormatVersion)
	{
		return VersionMismatch(
			source,
			CacheVersionField,
			WorkspaceCacheFormatVersion,
			std::to_string(cacheVersion));
	}

	const YAML::Node payloadVersionField = FindField(document, PayloadVersionField);
	if (!payloadVersionField.IsDefined())
	{
		return VersionMismatch(
			source,
			PayloadVersionField,
			expectedIdentity.m_payloadVersion,
			"missing");
	}

	uint32_t payloadVersion = 0;
	if (!ReadUint32(
			payloadVersionField,
			PayloadVersionField,
			source,
			payloadVersion,
			diagnostic))
	{
		return Fail(EWorkspaceCacheLoadStatus::Corrupt, std::move(diagnostic));
	}
	if (payloadVersion == 0 || payloadVersion != expectedIdentity.m_payloadVersion)
	{
		return VersionMismatch(
			source,
			PayloadVersionField,
			expectedIdentity.m_payloadVersion,
			std::to_string(payloadVersion));
	}

	if (!ValidateCurrentEnvelopeFields(document, source, diagnostic))
	{
		return Fail(EWorkspaceCacheLoadStatus::Corrupt, std::move(diagnostic));
	}

	std::string cacheKind;
	std::string producerIdentity;
	std::string workspaceId;
	std::string engineVersion;
	std::string buildIdentity;
	if (!ReadRequiredString(document, CacheKindField, source, cacheKind, diagnostic) ||
		!ReadRequiredString(document, ProducerIdentityField, source, producerIdentity, diagnostic) ||
		!ReadRequiredString(document, WorkspaceIdField, source, workspaceId, diagnostic) ||
		!ReadRequiredString(document, EngineVersionField, source, engineVersion, diagnostic) ||
		!ReadRequiredString(document, BuildIdentityField, source, buildIdentity, diagnostic))
	{
		return Fail(EWorkspaceCacheLoadStatus::Corrupt, std::move(diagnostic));
	}

	if (cacheKind != expectedIdentity.m_cacheKind)
	{
		return IdentityMismatch(source, CacheKindField, expectedIdentity.m_cacheKind, cacheKind);
	}
	if (producerIdentity != expectedIdentity.m_producerIdentity)
	{
		return IdentityMismatch(
			source,
			ProducerIdentityField,
			expectedIdentity.m_producerIdentity,
			producerIdentity);
	}
	if (workspaceId != expectedIdentity.m_workspaceId)
	{
		return IdentityMismatch(source, WorkspaceIdField, expectedIdentity.m_workspaceId, workspaceId);
	}
	if (engineVersion != expectedIdentity.m_engineVersion)
	{
		return IdentityMismatch(
			source,
			EngineVersionField,
			expectedIdentity.m_engineVersion,
			engineVersion);
	}
	if (buildIdentity != expectedIdentity.m_buildIdentity)
	{
		return IdentityMismatch(
			source,
			BuildIdentityField,
			expectedIdentity.m_buildIdentity,
			buildIdentity);
	}

	std::string payload;
	if (!ReadRequiredString(document, PayloadField, source, payload, diagnostic, true))
	{
		return Fail(EWorkspaceCacheLoadStatus::Corrupt, std::move(diagnostic));
	}

	WorkspaceCacheLoadResult result;
	result.m_status = EWorkspaceCacheLoadStatus::Loaded;
	result.m_diagnostic = source + " loaded with matching workspace and producer identity.";
	result.m_payload = std::move(payload);
	return result;
}

WorkspaceCacheLoadResult Sailor::Workspace::LoadWorkspaceCacheEnvelope(
	const std::filesystem::path& path,
	const WorkspaceCacheIdentity& expectedIdentity) noexcept
{
	std::error_code error;
	const bool exists = std::filesystem::exists(path, error);
	if (error)
	{
		return Fail(
			EWorkspaceCacheLoadStatus::IoFailure,
			"Cannot inspect workspace cache " + Quote(path.generic_string()) + ": " +
				error.message() + ".");
	}
	if (!exists)
	{
		return Fail(
			EWorkspaceCacheLoadStatus::Missing,
			"Workspace cache " + Quote(path.generic_string()) + " is missing.");
	}

	if (!std::filesystem::is_regular_file(path, error) || error)
	{
		return Fail(
			EWorkspaceCacheLoadStatus::IoFailure,
			"Workspace cache " + Quote(path.generic_string()) + " is not a readable regular file" +
				(error ? ": " + error.message() : std::string()) + ".");
	}

	std::ifstream stream(path, std::ios::binary);
	if (!stream.is_open())
	{
		return Fail(
			EWorkspaceCacheLoadStatus::IoFailure,
			"Cannot open workspace cache " + Quote(path.generic_string()) + ".");
	}

	std::ostringstream payload;
	payload << stream.rdbuf();
	if (stream.bad())
	{
		return Fail(
			EWorkspaceCacheLoadStatus::IoFailure,
			"Cannot read workspace cache " + Quote(path.generic_string()) + ".");
	}

	return ParseWorkspaceCacheEnvelope(payload.str(), expectedIdentity, path.generic_string());
}

bool Sailor::Workspace::AtomicReplaceWorkspaceCacheBinary(
	const std::filesystem::path& target,
	const void* data,
	uint64_t size,
	std::string& outDiagnostic,
	EWorkspaceCacheAtomicWriteFailurePoint failurePoint) noexcept
{
	outDiagnostic.clear();
	if (target.empty() || target.filename().empty())
	{
		outDiagnostic = "Cannot atomically replace workspace cache: the target path is empty or has no filename.";
		return false;
	}
	if (size > 0 && data == nullptr)
	{
		outDiagnostic = "Cannot atomically replace workspace cache " + Quote(target.generic_string()) +
			": non-empty data has a null address.";
		return false;
	}

	const std::filesystem::path parent = target.parent_path().empty()
		? std::filesystem::path(".")
		: target.parent_path();
	std::error_code directoryError;
	std::filesystem::create_directories(parent, directoryError);
	if (directoryError)
	{
		outDiagnostic = "Cannot create workspace cache directory " + Quote(parent.generic_string()) +
			": " + directoryError.message() + ".";
		return false;
	}

	const std::filesystem::path temporaryPath = MakeTemporaryPath(target);
	TemporaryFileCleanup cleanup(temporaryPath);
	if (!WriteTemporaryFile(temporaryPath, data, size, outDiagnostic))
	{
		return false;
	}

	if (failurePoint == EWorkspaceCacheAtomicWriteFailurePoint::BeforeReplace)
	{
		outDiagnostic = "Injected workspace cache replacement failure before replacing " +
			Quote(target.generic_string()) + ".";
		return false;
	}

#if defined(_WIN32)
	if (!MoveFileExW(
			temporaryPath.c_str(),
			target.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		const DWORD error = GetLastError();
		outDiagnostic = "Cannot atomically replace workspace cache " + Quote(target.generic_string()) +
			": " + WindowsErrorMessage(error) + ".";
		return false;
	}
	cleanup.Release();
#else
	if (rename(temporaryPath.c_str(), target.c_str()) != 0)
	{
		const int error = errno;
		outDiagnostic = "Cannot atomically replace workspace cache " + Quote(target.generic_string()) +
			": " + PosixErrorMessage(error) + ".";
		return false;
	}
	cleanup.Release();
	if (!FlushDirectory(parent, outDiagnostic))
	{
		return false;
	}
#endif

	outDiagnostic = "Atomically replaced workspace cache " + Quote(target.generic_string()) + ".";
	return true;
	return false;
}

bool Sailor::Workspace::AtomicReplaceWorkspaceCacheText(
	const std::filesystem::path& target,
	const std::string& text,
	std::string& outDiagnostic,
	EWorkspaceCacheAtomicWriteFailurePoint failurePoint) noexcept
{
	return AtomicReplaceWorkspaceCacheBinary(
		target,
		text.data(),
		static_cast<uint64_t>(text.size()),
		outDiagnostic,
		failurePoint);
}
