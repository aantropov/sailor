#include "Workspace/WorkspaceCacheContract.h"
#include "Workspace/WorkspaceContext.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <yaml-cpp/yaml.h>

using namespace Sailor::Workspace;

namespace
{
	class TempDirectory final
	{
	public:
		explicit TempDirectory(const char* label)
		{
			static uint64_t nextId = 0;
			const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
			m_path = std::filesystem::temp_directory_path() /
				("sailor-workspace-cache-" + std::string(label) + "-" +
					std::to_string(timestamp) + "-" + std::to_string(nextId++));
			std::filesystem::create_directories(m_path);
		}

		~TempDirectory() noexcept
		{
			std::error_code removeError;
			std::filesystem::remove_all(m_path, removeError);
		}

		const std::filesystem::path& Get() const noexcept { return m_path; }
		std::filesystem::path Path(const std::filesystem::path& relative) const
		{
			return m_path / relative;
		}

	private:
		std::filesystem::path m_path;
	};

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	std::string ReadText(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		Require(input.is_open(), "test file should be readable: " + path.generic_string());
		return std::string(
			std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>());
	}

	std::string GenericUtf8String(const std::filesystem::path& path)
	{
		const std::u8string utf8 = path.generic_u8string();
		return std::string(
			reinterpret_cast<const char*>(utf8.data()),
			utf8.size());
	}

#if defined(_WIN32)
	void FoldAsciiCaseForWindows(std::string& value)
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

	std::string Serialize(
		const WorkspaceCacheIdentity& identity,
		const std::string& payload = "entries:\n  - value: 42\n")
	{
		std::string envelope;
		std::string diagnostic;
		Require(
			SerializeWorkspaceCacheEnvelope(identity, payload, envelope, diagnostic),
			"test envelope should serialize: " + diagnostic);
		return envelope;
	}

	WorkspaceCacheIdentity Identity(
		const std::string& workspaceId = "workspace-a",
		const std::filesystem::path& root = "/workspace/a")
	{
		return MakeWorkspaceCacheIdentity(
			"asset-cache",
			"asset-cache-v1",
			1,
			workspaceId,
			root);
	}

	void RequireStatus(
		const WorkspaceCacheLoadResult& result,
		EWorkspaceCacheLoadStatus expected,
		const std::string& message)
	{
		Require(result.m_status == expected, message + ": " + result.m_diagnostic);
		if (expected != EWorkspaceCacheLoadStatus::Loaded)
		{
			Require(result.m_payload.empty(), message + " must not expose an unvalidated payload");
		}
	}

	void TestIdentityContract()
	{
		TempDirectory first("legacy-a");
		TempDirectory second("legacy-b");
		const std::string explicitId = ResolveWorkspaceCacheIdentity("manifest-id", first.Get());
		Require(explicitId == "manifest-id", "manifest workspace id should be preserved exactly");

		const std::string firstLegacy = ResolveWorkspaceCacheIdentity({}, first.Get());
		const std::string repeatedLegacy = ResolveWorkspaceCacheIdentity({}, first.Get() / ".");
		const std::string secondLegacy = ResolveWorkspaceCacheIdentity({}, second.Get());
		Require(firstLegacy.starts_with("legacy-root:"), "legacy identity should be explicitly namespaced");
		Require(firstLegacy == repeatedLegacy, "legacy identity should be stable for equivalent canonical roots");
		Require(firstLegacy != secondLegacy, "different legacy workspace roots must have different identities");

		const std::filesystem::path unicodeRoot = first.Get() /
			std::filesystem::path(u8"Legacy-РАБОЧАЯ-AZ");
		std::filesystem::create_directories(unicodeRoot);
		std::string expectedUnicodeRoot = GenericUtf8String(
			std::filesystem::weakly_canonical(unicodeRoot));
#if defined(_WIN32)
		FoldAsciiCaseForWindows(expectedUnicodeRoot);
#endif
		const std::string unicodeLegacy = ResolveWorkspaceCacheIdentity({}, unicodeRoot);
		Require(unicodeLegacy == "legacy-root:" + expectedUnicodeRoot,
			"legacy identity must use canonical UTF-8 and the shared ASCII-only Windows case fold");
		Require(unicodeLegacy.find(GenericUtf8String(std::filesystem::path(u8"РАБОЧАЯ"))) != std::string::npos,
			"legacy identity must preserve non-ASCII UTF-8 bytes exactly");

		const WorkspaceCacheIdentity identity = Identity();
		Require(identity.m_cacheVersion == WorkspaceCacheFormatVersion,
			"identity should use the current common cache format");
		Require(identity.m_payloadVersion == 1, "identity should preserve the payload version");
		Require(identity.m_cacheKind == "asset-cache", "identity should preserve the cache kind");
		Require(identity.m_producerIdentity == "asset-cache-v1",
			"identity should preserve the producer identity");
		Require(identity.m_workspaceId == "workspace-a", "identity should preserve manifest workspace id");
		Require(!identity.m_engineVersion.empty() && identity.m_engineVersion != "unknown",
			"engine version should come from the CMake project version");
		Require(identity.m_buildIdentity.find("config=") != std::string::npos,
			"build identity should contain the CMake build configuration");
		Require(identity.m_buildIdentity.find("sailor-workspace-abi-") != std::string::npos,
			"build identity should contain the existing workspace ABI tag");

		const WorkspaceContextResolveResult contextResult = ResolveWorkspaceContext(first.Get());
		Require(contextResult.IsSuccess(), "legacy context should resolve for cache identity test");
		const WorkspaceCacheIdentity contextIdentity = MakeWorkspaceCacheIdentity(
			"asset-cache",
			"asset-cache-v1",
			1,
			contextResult.m_context);
		Require(contextIdentity.m_workspaceId == firstLegacy,
			"context overload should derive the same deterministic legacy identity");
	}

	void TestRoundTripAndFileStatuses()
	{
		TempDirectory directory("roundtrip");
		const WorkspaceCacheIdentity identity = Identity();
		const std::string payload = "entries:\n  - value: 42\n  - value: cache payload\n";
		const std::string envelope = Serialize(identity, payload);

		const WorkspaceCacheLoadResult parsed = ParseWorkspaceCacheEnvelope(
			envelope,
			identity,
			"memory fixture");
		RequireStatus(parsed, EWorkspaceCacheLoadStatus::Loaded, "matching envelope should load");
		Require(parsed.IsLoaded(), "loaded result helper should report success");
		Require(parsed.m_payload == payload, "payload should round-trip exactly as an envelope scalar");

		const std::filesystem::path cachePath = directory.Path("Nested/AssetCache.yaml");
		WorkspaceCacheLoadResult missing = LoadWorkspaceCacheEnvelope(cachePath, identity);
		RequireStatus(missing, EWorkspaceCacheLoadStatus::Missing, "missing cache should be distinguished");

		std::string diagnostic;
		Require(
			AtomicReplaceWorkspaceCacheText(cachePath, envelope, diagnostic),
			"cache envelope should be atomically writable: " + diagnostic);
		const WorkspaceCacheLoadResult loaded = LoadWorkspaceCacheEnvelope(cachePath, identity);
		RequireStatus(loaded, EWorkspaceCacheLoadStatus::Loaded, "written cache should load");
		Require(loaded.m_payload == payload, "file load should return the complete payload");

		const WorkspaceCacheLoadResult ioFailure = LoadWorkspaceCacheEnvelope(directory.Get(), identity);
		RequireStatus(ioFailure, EWorkspaceCacheLoadStatus::IoFailure,
			"directory in place of a cache file should be an I/O failure");
	}

	void TestIdentityMismatches()
	{
		const WorkspaceCacheIdentity expected = Identity();
		struct Mismatch
		{
			const char* m_field;
			std::string WorkspaceCacheIdentity::* m_value;
			const char* m_actual;
		};
		const Mismatch mismatches[] =
		{
			{ "cacheKind", &WorkspaceCacheIdentity::m_cacheKind, "shader-cache" },
			{ "producerIdentity", &WorkspaceCacheIdentity::m_producerIdentity, "asset-cache-v2" },
			{ "workspaceId", &WorkspaceCacheIdentity::m_workspaceId, "workspace-b" },
			{ "engineVersion", &WorkspaceCacheIdentity::m_engineVersion, "other-engine" },
			{ "buildIdentity", &WorkspaceCacheIdentity::m_buildIdentity, "other-build" }
		};

		for (const Mismatch& mismatch : mismatches)
		{
			WorkspaceCacheIdentity actual = expected;
			actual.*(mismatch.m_value) = mismatch.m_actual;
			const WorkspaceCacheLoadResult result = ParseWorkspaceCacheEnvelope(
				Serialize(actual),
				expected,
				"identity fixture");
			RequireStatus(result, EWorkspaceCacheLoadStatus::StaleIdentity,
				std::string(mismatch.m_field) + " mismatch should be stale");
			Require(result.m_diagnostic.find(mismatch.m_field) != std::string::npos,
				"stale diagnostic should name field " + std::string(mismatch.m_field));
			Require(result.m_diagnostic.find(mismatch.m_actual) != std::string::npos,
				"stale diagnostic should contain actual value for " + std::string(mismatch.m_field));
			Require(result.m_diagnostic.find(expected.*(mismatch.m_value)) != std::string::npos,
				"stale diagnostic should contain expected value for " + std::string(mismatch.m_field));
		}
	}

	void TestUnsupportedVersions()
	{
		const WorkspaceCacheIdentity expected = Identity();

		WorkspaceCacheLoadResult raw = ParseWorkspaceCacheEnvelope(
			"assets: []\n",
			expected,
			"raw legacy cache");
		RequireStatus(raw, EWorkspaceCacheLoadStatus::UnsupportedVersion,
			"raw legacy payload should be unsupported rather than accepted");
		Require(raw.m_diagnostic.find("expected '1'") != std::string::npos &&
			raw.m_diagnostic.find("actual 'missing'") != std::string::npos,
			"missing format diagnostic should contain expected and actual version");

		YAML::Node futureDocument = YAML::Load(Serialize(expected));
		futureDocument["cacheVersion"] = WorkspaceCacheFormatVersion + 1;
		WorkspaceCacheLoadResult future = ParseWorkspaceCacheEnvelope(
			YAML::Dump(futureDocument),
			expected,
			"future cache");
		RequireStatus(future, EWorkspaceCacheLoadStatus::UnsupportedVersion,
			"future cache format should be unsupported");
		Require(future.m_diagnostic.find("actual '2'") != std::string::npos,
			"future format diagnostic should contain actual version");

		YAML::Node payloadDocument = YAML::Load(Serialize(expected));
		payloadDocument["payloadVersion"] = 2;
		WorkspaceCacheLoadResult payloadVersion = ParseWorkspaceCacheEnvelope(
			YAML::Dump(payloadDocument),
			expected,
			"future payload");
		RequireStatus(payloadVersion, EWorkspaceCacheLoadStatus::UnsupportedVersion,
			"future payload version should be unsupported");
		Require(payloadVersion.m_diagnostic.find("payloadVersion") != std::string::npos &&
			payloadVersion.m_diagnostic.find("expected '1'") != std::string::npos &&
			payloadVersion.m_diagnostic.find("actual '2'") != std::string::npos,
			"payload version diagnostic should contain field, expected, and actual values");

		payloadDocument.remove("payloadVersion");
		WorkspaceCacheLoadResult missingPayloadVersion = ParseWorkspaceCacheEnvelope(
			YAML::Dump(payloadDocument),
			expected,
			"legacy envelope");
		RequireStatus(missingPayloadVersion, EWorkspaceCacheLoadStatus::UnsupportedVersion,
			"missing payload version should be unsupported");
	}

	void TestCorruptEnvelopes()
	{
		const WorkspaceCacheIdentity expected = Identity();
		WorkspaceCacheLoadResult malformed = ParseWorkspaceCacheEnvelope(
			"cacheVersion: [1\n",
			expected,
			"truncated cache");
		RequireStatus(malformed, EWorkspaceCacheLoadStatus::Corrupt,
			"truncated YAML should be corrupt");

		const std::string valid = Serialize(expected);
		WorkspaceCacheLoadResult duplicate = ParseWorkspaceCacheEnvelope(
			"cacheVersion: 1\n" + valid,
			expected,
			"duplicate cache");
		RequireStatus(duplicate, EWorkspaceCacheLoadStatus::Corrupt,
			"duplicate envelope field should be corrupt");

		YAML::Node unknownDocument = YAML::Load(valid);
		unknownDocument["unexpected"] = "value";
		WorkspaceCacheLoadResult unknown = ParseWorkspaceCacheEnvelope(
			YAML::Dump(unknownDocument),
			expected,
			"unknown-field cache");
		RequireStatus(unknown, EWorkspaceCacheLoadStatus::Corrupt,
			"unknown field in current envelope should be corrupt");

		YAML::Node missingDocument = YAML::Load(valid);
		missingDocument.remove("workspaceId");
		WorkspaceCacheLoadResult missing = ParseWorkspaceCacheEnvelope(
			YAML::Dump(missingDocument),
			expected,
			"missing-field cache");
		RequireStatus(missing, EWorkspaceCacheLoadStatus::Corrupt,
			"missing identity field should be corrupt");

		YAML::Node invalidPayload = YAML::Load(valid);
		invalidPayload["payload"] = YAML::Node(YAML::NodeType::Map);
		invalidPayload["payload"]["entry"] = 1;
		WorkspaceCacheLoadResult nonScalarPayload = ParseWorkspaceCacheEnvelope(
			YAML::Dump(invalidPayload),
			expected,
			"invalid-payload cache");
		RequireStatus(nonScalarPayload, EWorkspaceCacheLoadStatus::Corrupt,
			"non-scalar envelope payload should be corrupt");
	}

	void TestAtomicReplacementAndInjectedFailure()
	{
		TempDirectory directory("atomic");
		const std::filesystem::path target = directory.Path("Cache/AssetCache.yaml");
		std::string diagnostic;
		Require(
			AtomicReplaceWorkspaceCacheText(target, "old-cache", diagnostic),
			"initial atomic write should succeed: " + diagnostic);
		Require(ReadText(target) == "old-cache", "initial cache contents should be complete");

		Require(
			!AtomicReplaceWorkspaceCacheText(
				target,
				"new-cache-that-must-not-appear",
				diagnostic,
				EWorkspaceCacheAtomicWriteFailurePoint::BeforeReplace),
			"injected pre-replace failure should be reported");
		Require(diagnostic.find("Injected") != std::string::npos,
			"injected failure should have a targeted diagnostic");
		Require(ReadText(target) == "old-cache",
			"failed replacement must preserve the complete previous target");

		for (const auto& entry : std::filesystem::directory_iterator(target.parent_path()))
		{
			Require(entry.path().extension() != ".tmp",
				"failed replacement must clean its same-directory temporary file");
		}

		const uint8_t binary[] = { 0, 1, 2, 3, 4, 5, 255 };
		Require(
			AtomicReplaceWorkspaceCacheBinary(target, binary, sizeof(binary), diagnostic),
			"binary atomic replacement should succeed: " + diagnostic);
		std::ifstream input(target, std::ios::binary);
		const std::string actual{
			std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>() };
		Require(actual.size() == sizeof(binary), "binary atomic replacement should preserve exact size");
		Require(std::equal(actual.begin(), actual.end(), reinterpret_cast<const char*>(binary)),
			"binary atomic replacement should preserve exact bytes");

		for (const auto& entry : std::filesystem::directory_iterator(target.parent_path()))
		{
			Require(entry.path().extension() != ".tmp",
				"successful replacement must not leave a temporary file");
		}
	}
}

int main()
{
	try
	{
		TestIdentityContract();
		TestRoundTripAndFileStatuses();
		TestIdentityMismatches();
		TestUnsupportedVersions();
		TestCorruptEnvelopes();
		TestAtomicReplacementAndInjectedFailure();
		std::cout << "[PASS] Workspace cache contract" << std::endl;
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "[FAIL] Workspace cache contract: " << e.what() << std::endl;
		return 1;
	}
}
