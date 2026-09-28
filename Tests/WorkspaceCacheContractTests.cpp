#include "Workspace/WorkspaceCacheContract.h"
#include "Workspace/WorkspaceContext.h"
#include "Platform/AtomicFile.h"
#include "Platform/AtomicFileTestAccess.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <latch>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <yaml-cpp/yaml.h>

#if defined(_WIN32)
#include <Windows.h>
#endif

using namespace Sailor::Workspace;
using namespace Sailor::Platform;

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
			AtomicWriteFile(cachePath, envelope, diagnostic) == EAtomicWriteResult::Synced,
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

	void RequireNoTemporaryFiles(const std::filesystem::path& directory)
	{
		for (const auto& entry : std::filesystem::directory_iterator(directory))
		{
			Require(entry.path().extension() != ".tmp", "atomic write must clean its temporary file");
		}
	}

	void TestAtomicReplacementAndInjectedFailure()
	{
		TempDirectory directory("atomic");
		const auto target = directory.Path("Cache/AssetCache.yaml");
		std::string diagnostic;
		Require(AtomicWriteFile(target, "old-cache", diagnostic) == EAtomicWriteResult::Synced,
			"initial atomic write must sync: " + diagnostic);
		Require(ReadText(target) == "old-cache", "initial target must contain complete bytes");

		const std::string replacement = "new-cache-that-must-not-appear";
		Require(AtomicWriteFileForTests(target, replacement.data(), replacement.size(), diagnostic,
			EAtomicWriteFailurePoint::BeforePublish) == EAtomicWriteResult::NotPublished,
			"pre-publish failure must report no publication");
		Require(ReadText(target) == "old-cache", "pre-publish failure must preserve the previous target");
		RequireNoTemporaryFiles(target.parent_path());

		const uint8_t binary[] = { 0, 1, 2, 3, 4, 5, 255 };
		Require(AtomicWriteFile(target, binary, sizeof(binary), diagnostic) == EAtomicWriteResult::Synced,
			"binary replacement must sync: " + diagnostic);
		const auto actual = ReadText(target);
		Require(actual.size() == sizeof(binary) &&
			std::equal(actual.begin(), actual.end(), reinterpret_cast<const char*>(binary)),
			"binary replacement must preserve exact bytes");
		Require(AtomicWriteFile(target, nullptr, 0, diagnostic) == EAtomicWriteResult::Synced &&
			ReadText(target).empty(), "empty files must publish without a data pointer");
		Require(AtomicWriteFile(target, nullptr, 1, diagnostic) == EAtomicWriteResult::NotPublished &&
			ReadText(target).empty(), "invalid input must not replace the current target");
		RequireNoTemporaryFiles(target.parent_path());
	}

	void TestPostPublishResult()
	{
		for (const auto mode : { EAtomicWriteMode::ReplaceExisting, EAtomicWriteMode::FailIfExists })
		{
			TempDirectory directory("post-publish");
			const auto target = directory.Path("output.bin");
			std::string diagnostic;
			if (mode == EAtomicWriteMode::ReplaceExisting)
			{
				Require(AtomicWriteFile(target, "previous", diagnostic) == EAtomicWriteResult::Synced,
					"post-publish fixture must initialize");
			}
			const std::string data = "published despite the sync failure";
			const auto result = AtomicWriteFileForTests(target, data.data(), data.size(), diagnostic,
				EAtomicWriteFailurePoint::DirectorySync, mode);
			Require(ReadText(target) == data, "a post-publish failure must leave the complete new target visible");
			Require(result == EAtomicWriteResult::Published,
				"directory sync failure must report Published, not NotPublished or Synced");
			Require(!IsAtomicWriteComplete(result), "a real sync failure must not acknowledge completed persistence");
			Require(!diagnostic.empty(), "unconfirmed sync must report a diagnostic");
			RequireNoTemporaryFiles(directory.Get());

			Require(AtomicWriteFile(target, "do not overwrite", diagnostic, EAtomicWriteMode::FailIfExists) ==
				EAtomicWriteResult::NotPublished && ReadText(target) == data,
				"exclusive retry must never overwrite an already published target");
			Require(AtomicWriteFile(target, data, diagnostic) == EAtomicWriteResult::Synced,
				"replace retry must complete pending synchronization");
			RequireNoTemporaryFiles(directory.Get());
		}
#if !defined(_WIN32)
		TempDirectory directory("link-cleanup");
		const auto target = directory.Path("output.bin");
		std::string diagnostic;
		const std::string data = "exclusive publication";
		Require(AtomicWriteFileForTests(target, data.data(), data.size(), diagnostic,
			EAtomicWriteFailurePoint::TemporaryCleanup, EAtomicWriteMode::FailIfExists) == EAtomicWriteResult::Synced,
			"temporary-link cleanup failure must not skip directory sync or undo publication");
		Require(ReadText(target) == data, "cleanup failure must preserve the complete published target");
		RequireNoTemporaryFiles(directory.Get());
		const auto unsupported = AtomicWriteFileForTests(target, data.data(), data.size(), diagnostic,
			EAtomicWriteFailurePoint::DirectorySyncUnsupported);
		Require(unsupported == EAtomicWriteResult::DirectorySyncUnsupported && IsAtomicWriteComplete(unsupported),
			"unsupported directory sync must retain best-effort success without claiming a synced directory");
		Require(ReadText(target) == data, "best-effort publication must retain the complete target");
		RequireNoTemporaryFiles(directory.Get());
#endif
	}

	std::string ReadWhileReplacing(const std::filesystem::path& path)
	{
#if defined(_WIN32)
		const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		Require(file != INVALID_HANDLE_VALUE, "concurrent reader must open the current target");
		std::array<char, 65536> bytes{};
		DWORD read = 0;
		const bool success = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != 0;
		CloseHandle(file);
		Require(success, "concurrent reader must finish reading its opened generation");
		return { bytes.data(), read };
#else
		return ReadText(path);
#endif
	}

	void TestConcurrentPublication()
	{
		TempDirectory directory("concurrent");
		const auto exclusive = directory.Path("exclusive.bin");
		const std::array<std::string, 4> payloads = {
			std::string(16381, 'A'), std::string(24577, 'B'),
			std::string(32769, 'C'), std::string(49151, 'D') };
		std::array<EAtomicWriteResult, 4> results{};
		std::array<std::jthread, 4> writers;
		std::latch start(1);
		for (size_t i = 0; i < writers.size(); ++i)
		{
			writers[i] = std::jthread([&, i]()
			{
				start.wait();
				std::string diagnostic;
				results[i] = AtomicWriteFile(exclusive, payloads[i], diagnostic, EAtomicWriteMode::FailIfExists);
			});
		}
		start.count_down();
		for (auto& writer : writers)
		{
			writer.join();
		}
		size_t published = 0;
		for (size_t i = 0; i < results.size(); ++i)
		{
			if (results[i] != EAtomicWriteResult::NotPublished)
			{
				++published;
				Require(results[i] == EAtomicWriteResult::Synced && ReadText(exclusive) == payloads[i],
					"exclusive winner must publish and sync exactly its payload");
			}
		}
		Require(published == 1, "exactly one competing exclusive writer must publish");
		RequireNoTemporaryFiles(directory.Get());

		const auto target = directory.Path("replace.bin");
		std::string diagnostic;
		Require(AtomicWriteFile(target, payloads[0], diagnostic) == EAtomicWriteResult::Synced,
			"concurrent replacement fixture must initialize");
		std::atomic<bool> valid{ true };
		std::atomic<uint32_t> reads{ 0 };
		std::latch readerStarted(1);
		std::jthread reader([&](std::stop_token stop)
		{
			while (!stop.stop_requested())
			{
				try
				{
					const auto value = ReadWhileReplacing(target);
					if (std::find(payloads.begin(), payloads.end(), value) == payloads.end())
					{
						valid = false;
					}
				}
				catch (...)
				{
					valid = false;
				}
				if (reads.fetch_add(1) == 0)
				{
					readerStarted.count_down();
				}
			}
		});
		readerStarted.wait();
		std::latch replaceStart(1);
		for (size_t i = 0; i < writers.size(); ++i)
		{
			writers[i] = std::jthread([&, i]()
			{
				replaceStart.wait();
				for (int attempt = 0; attempt < 8; ++attempt)
				{
					std::string writeDiagnostic;
					if (AtomicWriteFile(target, payloads[i], writeDiagnostic) != EAtomicWriteResult::Synced)
					{
						valid = false;
					}
				}
			});
		}
		replaceStart.count_down();
		for (auto& writer : writers)
		{
			writer.join();
		}
		reader.request_stop();
		reader.join();
		Require(valid && reads > 0, "concurrent readers must see only complete old or new payloads");
		const auto final = ReadText(target);
		Require(std::find(payloads.begin(), payloads.end(), final) != payloads.end(),
			"the last replacement must be a complete writer payload");
		RequireNoTemporaryFiles(directory.Get());
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
		TestPostPublishResult();
		TestConcurrentPublication();
		std::cout << "[PASS] Workspace cache contract" << std::endl;
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "[FAIL] Workspace cache contract: " << e.what() << std::endl;
		return 1;
	}
}
