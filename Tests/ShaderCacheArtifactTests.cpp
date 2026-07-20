#include "AssetRegistry/Shader/ShaderCache.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "Workspace/WorkspaceCacheContract.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <yaml-cpp/yaml.h>

namespace
{
	using namespace Sailor;

	class TempDirectory final
	{
	public:
		TempDirectory()
		{
			static uint64_t nextId = 0;
			const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
			m_path = std::filesystem::temp_directory_path() /
				("sailor-shader-cache-artifacts-" + std::to_string(timestamp) + "-" +
					std::to_string(nextId++));
			std::filesystem::create_directories(m_path);
		}

		~TempDirectory() noexcept
		{
			std::error_code error;
			std::filesystem::remove_all(m_path, error);
		}

		std::filesystem::path Path(const char* filename) const
		{
			return m_path / filename;
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

	FileId MakeFileId(const std::string& value)
	{
		FileId result;
		result.Deserialize(YAML::Node(value));
		return result;
	}

	TVector<uint32_t> Words(uint32_t seed)
	{
		TVector<uint32_t> result;
		result.Add(0x07230203);
		result.Add(seed);
		result.Add(seed ^ 0x5a5a5a5a);
		result.Add(seed + 17);
		return result;
	}

	std::string ReadText(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		Require(input.is_open(), "test text should be readable: " + path.generic_string());
		return std::string(
			std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>());
	}

	size_t CountRegularFiles(const std::filesystem::path& root)
	{
		std::error_code error;
		size_t result = 0;
		for (std::filesystem::recursive_directory_iterator iterator(root, error), end;
			!error && iterator != end;
			iterator.increment(error))
		{
			if (iterator->is_regular_file(error))
			{
				++result;
			}
		}
		Require(!error, "test storage should be enumerable: " + error.message());
		return result;
	}

	void RequireWords(
		const TVector<uint32_t>& actual,
		const TVector<uint32_t>& expected,
		const std::string& context)
	{
		Require(actual.Num() == expected.Num(), context + " should preserve the word count");
		for (size_t index = 0; index < expected.Num(); ++index)
		{
			Require(actual[index] == expected[index], context + " should preserve every word");
		}
	}

	void WriteWords(const std::filesystem::path& path, const TVector<uint32_t>& words)
	{
		std::string diagnostic;
		Require(
			Workspace::AtomicReplaceWorkspaceCacheBinary(
				path,
				words.Num() == 0 ? nullptr : &words[0],
				static_cast<uint64_t>(words.Num()) * sizeof(uint32_t),
				diagnostic),
			"test SPIR-V should be writable: " + diagnostic);
	}

	bool PublishComplete(
		ShaderCache& cache,
		const FileId& uid,
		uint32_t permutation,
		uint32_t seed)
	{
		const TVector<uint32_t> empty;
		return cache.CacheSpirv_ThreadSafe(
			uid,
			permutation,
			Words(seed),
			Words(seed + 1),
			empty,
			Words(seed + 2),
			Words(seed + 3),
			empty);
	}

	template<size_t Size>
	void WriteArtifact(const std::filesystem::path& path, const std::array<uint8_t, Size>& bytes)
	{
		std::string diagnostic;
		Require(
			Workspace::AtomicReplaceWorkspaceCacheBinary(
				path,
				bytes.data(),
				bytes.size(),
				diagnostic),
			"test artifact should be writable: " + diagnostic);
	}

	TVector<uint32_t> SentinelOutput()
	{
		TVector<uint32_t> result;
		result.Add(0xdecafbad);
		result.Add(0xabcdef01);
		return result;
	}

	void RequireSentinel(const TVector<uint32_t>& output, const std::string& context)
	{
		Require(
			output.Num() == 2 && output[0] == 0xdecafbad && output[1] == 0xabcdef01,
			context + " must not partially replace caller output");
	}

	void TestArtifactRoundTrip()
	{
		TempDirectory directory;
		const std::array<uint32_t, 4> words =
		{
			0x07230203,
			0x00010000,
			0x000d000b,
			0x00000001
		};
		const auto metadata = ShaderCache::DescribeArtifact(words.data(), sizeof(words));
		const auto path = directory.Path("valid.spirv");
		std::string writeDiagnostic;
		Require(
			Workspace::AtomicReplaceWorkspaceCacheBinary(
				path,
				words.data(),
				sizeof(words),
				writeDiagnostic),
			"valid SPIR-V fixture should be writable: " + writeDiagnostic);

		TVector<uint32_t> output = SentinelOutput();
		std::string diagnostic;
		Require(
			ShaderCache::ReadSpirvArtifact(path, metadata, output, diagnostic),
			"matching artifact should load: " + diagnostic);
		Require(output.Num() == words.size(), "matching artifact should preserve its word count");
		for (size_t index = 0; index < words.size(); ++index)
		{
			Require(output[index] == words[index], "matching artifact should preserve every word");
		}
	}

	void TestTruncatedArtifactIsRejected()
	{
		TempDirectory directory;
		const std::array<uint8_t, 8> complete = { 3, 2, 35, 7, 0, 0, 1, 0 };
		const std::array<uint8_t, 4> truncated = { 3, 2, 35, 7 };
		const auto metadata = ShaderCache::DescribeArtifact(complete.data(), complete.size());
		const auto path = directory.Path("truncated.spirv");
		WriteArtifact(path, truncated);

		TVector<uint32_t> output = SentinelOutput();
		std::string diagnostic;
		Require(
			!ShaderCache::ReadSpirvArtifact(path, metadata, output, diagnostic),
			"truncated artifact should be rejected");
		Require(diagnostic.find("byte length") != std::string::npos,
			"truncated artifact diagnostic should identify its byte length");
		RequireSentinel(output, "truncated artifact failure");
	}

	void TestMisalignedArtifactIsRejected()
	{
		TempDirectory directory;
		const std::array<uint8_t, 3> bytes = { 3, 2, 35 };
		const auto metadata = ShaderCache::DescribeArtifact(bytes.data(), bytes.size());
		const auto path = directory.Path("misaligned.spirv");
		WriteArtifact(path, bytes);

		TVector<uint32_t> output = SentinelOutput();
		std::string diagnostic;
		Require(
			!ShaderCache::ReadSpirvArtifact(path, metadata, output, diagnostic),
			"misaligned artifact should be rejected");
		Require(diagnostic.find("aligned") != std::string::npos,
			"misaligned artifact diagnostic should identify word alignment");
		RequireSentinel(output, "misaligned artifact failure");
	}

	void TestChecksumMismatchIsRejected()
	{
		TempDirectory directory;
		const std::array<uint8_t, 8> bytes = { 3, 2, 35, 7, 0, 0, 1, 0 };
		auto metadata = ShaderCache::DescribeArtifact(bytes.data(), bytes.size());
		metadata.m_checksum ^= 1;
		const auto path = directory.Path("checksum.spirv");
		WriteArtifact(path, bytes);

		TVector<uint32_t> output = SentinelOutput();
		std::string diagnostic;
		Require(
			!ShaderCache::ReadSpirvArtifact(path, metadata, output, diagnostic),
			"checksum mismatch should be rejected");
		Require(diagnostic.find("checksum mismatch") != std::string::npos,
			"checksum mismatch diagnostic should identify checksum validation");
		RequireSentinel(output, "checksum mismatch failure");
	}

	void TestOwnedArtifactContainment()
	{
		TempDirectory directory;
		const std::filesystem::path cacheRoot = directory.Path("Cache");
		const std::filesystem::path ownedDirectory = cacheRoot / "CompiledShaders";
		std::filesystem::create_directories(ownedDirectory);

		std::string diagnostic;
		Require(
			ShaderCache::ValidateOwnedArtifactPath(
				cacheRoot,
				ownedDirectory,
				ownedDirectory / "direct.spirv",
				diagnostic),
			"a direct child artifact should be accepted: " + diagnostic);

		diagnostic.clear();
		Require(
			!ShaderCache::ValidateOwnedArtifactPath(
				cacheRoot,
				ownedDirectory,
				ownedDirectory / ".." / "neighbor.txt",
				diagnostic),
			"a traversal artifact path should be rejected");
		Require(diagnostic.find("outside owned directory") != std::string::npos,
			"traversal diagnostic should identify the owned-directory boundary");

		const std::filesystem::path symlinkTarget = cacheRoot / "Neighbor";
		std::filesystem::create_directories(symlinkTarget);
		std::filesystem::remove_all(ownedDirectory);
		std::error_code symlinkError;
		std::filesystem::create_directory_symlink(symlinkTarget, ownedDirectory, symlinkError);
		if (!symlinkError)
		{
			diagnostic.clear();
			Require(
				!ShaderCache::ValidateOwnedArtifactPath(
					cacheRoot,
					ownedDirectory,
					ownedDirectory / "linked.spirv",
					diagnostic),
				"a symlinked owned directory should be rejected");
			Require(diagnostic.find("symlinked") != std::string::npos,
				"symlink diagnostic should identify the unsafe directory");
		}
	}

	void TestDebugArtifactsAreRequired()
	{
		TempDirectory directory;
		ShaderCache cache;
		Require(ShaderCacheTestAccess::Configure(cache, directory.Path("Cache")),
			"shader cache test storage should initialize");
		const FileId uid = MakeFileId("{SHADER-CACHE-DEBUG-REQUIRED}");

		Require(PublishComplete(cache, uid, 0, 10),
			"a complete regular/debug generation should publish");
		cache.SaveCache(true);
		Require(!cache.IsDirty(), "a complete generation should save before payload validation");

		const std::string payload = ShaderCacheTestAccess::PayloadWithMissingDebug(cache);
		std::string diagnostic;
		Require(!ShaderCacheTestAccess::ParsePayload(payload, diagnostic),
			"payload v2 must reject entries without required debug artifacts");
		Require(diagnostic.find("debug artifacts") != std::string::npos,
			"missing-debug diagnostic should identify the required debug artifact set");

		diagnostic.clear();
		Require(
			!ShaderCacheTestAccess::ParsePayload(
				ShaderCacheTestAccess::PayloadWithMismatchedDebugTopology(cache),
				diagnostic),
			"payload v2 must reject different regular/debug shader stage topology");
		Require(diagnostic.find("identical shader stages") != std::string::npos,
			"topology diagnostic should identify the regular/debug stage mismatch");

		const TVector<uint32_t> empty;
		Require(
			!cache.CacheSpirv_ThreadSafe(
				uid,
				1,
				Words(11),
				Words(12),
				empty,
				empty,
				empty,
				Words(13)),
			"live publication must reject different regular/debug shader stage topology");
	}

	void TestFailedGenerationPreservesDurableGeneration()
	{
		TempDirectory directory;
		const std::filesystem::path cacheRoot = directory.Path("Cache");
		ShaderCache cache;
		Require(ShaderCacheTestAccess::Configure(cache, cacheRoot),
			"shader cache test storage should initialize");
		const FileId uid = MakeFileId("{SHADER-CACHE-GENERATION-TRANSACTION}");

		Require(PublishComplete(cache, uid, 3, 20), "the initial generation should publish");
		cache.SaveCache(true);
		Require(!cache.IsDirty(), "the initial generation should be durable");

		const std::string durableGeneration = ShaderCacheTestAccess::GetGeneration(cache, uid, 3);
		Require(!durableGeneration.empty(), "the durable generation should expose test metadata");
		const auto durableVertexPath = ShaderCacheTestAccess::GetArtifactPath(
			cache,
			uid,
			3,
			ShaderCache::VertexShaderTag,
			false);
		const auto cachePath = ShaderCacheTestAccess::GetCachePath(cache);
		const std::string durableEnvelope = ReadText(cachePath);
		const size_t durableFileCount = CountRegularFiles(cacheRoot);

		const TVector<uint32_t> empty;
		std::string diagnostic;
		Require(
			!ShaderCacheTestAccess::PublishWithArtifactFailure(
				cache,
				uid,
				3,
				Words(30),
				Words(31),
				empty,
				Words(32),
				Words(33),
				empty,
				1,
				diagnostic),
			"an injected artifact replacement failure should reject the new generation");
		Require(!diagnostic.empty(), "failed generation publication should report a diagnostic");

		Require(ShaderCacheTestAccess::GetGeneration(cache, uid, 3) == durableGeneration,
			"failed generation publication must retain the durable generation metadata");
		Require(ReadText(cachePath) == durableEnvelope,
			"failed generation publication must not replace the durable envelope");
		Require(std::filesystem::exists(durableVertexPath),
			"failed generation publication must preserve durable artifacts");

		TVector<uint32_t> loadedVertex;
		TVector<uint32_t> loadedFragment;
		TVector<uint32_t> loadedCompute;
		Require(cache.GetSpirvCode(uid, 3, loadedVertex, loadedFragment, loadedCompute),
			"the durable generation should remain readable after failed publication");
		RequireWords(loadedVertex, Words(20), "the durable generation");

		Require(CountRegularFiles(cacheRoot) > durableFileCount,
			"the failure seam should leave an uncommitted immutable artifact for cleanup");
		cache.ClearExpired();
		Require(CountRegularFiles(cacheRoot) == durableFileCount,
			"cleanup should sweep only the uncommitted generation");
		Require(std::filesystem::exists(durableVertexPath),
			"cleanup must whitelist artifacts from the committed metadata snapshot");
		Require(ReadText(cachePath) == durableEnvelope,
			"orphan cleanup should not rewrite unchanged committed metadata");
	}

	void TestRemoveCommitsBeforeGarbageCollection()
	{
		TempDirectory directory;
		ShaderCache cache;
		Require(ShaderCacheTestAccess::Configure(cache, directory.Path("Cache")),
			"shader cache test storage should initialize");
		const FileId uid = MakeFileId("{SHADER-CACHE-REMOVE-TRANSACTION}");

		Require(PublishComplete(cache, uid, 0, 40), "the removable generation should publish");
		cache.SaveCache(true);
		const auto artifactPath = ShaderCacheTestAccess::GetArtifactPath(
			cache,
			uid,
			0,
			ShaderCache::VertexShaderTag,
			false);
		const auto cachePath = ShaderCacheTestAccess::GetCachePath(cache);
		const std::string durableEnvelope = ReadText(cachePath);

		std::string diagnostic;
		Require(!ShaderCacheTestAccess::RemoveWithEnvelopeFailure(cache, uid, diagnostic),
			"remove should fail when its metadata replacement is injected to fail");
		Require(cache.Contains(uid), "failed remove must retain its live metadata");
		Require(std::filesystem::exists(artifactPath),
			"failed remove must not garbage-collect a durably referenced artifact");
		Require(ReadText(cachePath) == durableEnvelope,
			"failed remove must preserve the durable envelope");

		ShaderCacheTestAccess::FailNextArtifactSweep(cache);
		cache.Remove(uid);
		Require(!cache.Contains(uid) && cache.IsDirty(),
			"remove should retain retryable dirty state when post-commit cleanup fails");
		Require(std::filesystem::exists(artifactPath),
			"failed post-commit cleanup should retain the now-unreferenced artifact");
		cache.SaveCache();
		Require(!cache.IsDirty() && !std::filesystem::exists(artifactPath),
			"save retry should garbage-collect removed artifacts after the committed metadata");
	}

	void TestSameSizeChecksumCorruptionAndClearExpiredTransaction()
	{
		TempDirectory directory;
		ShaderCache cache;
		Require(ShaderCacheTestAccess::Configure(cache, directory.Path("Cache")),
			"shader cache test storage should initialize");
		const FileId uid = MakeFileId("{SHADER-CACHE-CHECKSUM-EXPIRY}");

		Require(PublishComplete(cache, uid, 1, 50), "the corruption fixture should publish");
		cache.SaveCache(true);
		const auto artifactPath = ShaderCacheTestAccess::GetArtifactPath(
			cache,
			uid,
			1,
			ShaderCache::VertexShaderTag,
			false);
		const auto cachePath = ShaderCacheTestAccess::GetCachePath(cache);
		const std::string durableEnvelope = ReadText(cachePath);
		const std::string corruptGeneration = ShaderCacheTestAccess::GetGeneration(cache, uid, 1);

		WriteWords(artifactPath, Words(500));
		Require(cache.IsExpired(uid, 1),
			"same-size checksum corruption must expire a cached generation");

		std::string diagnostic;
		Require(!ShaderCacheTestAccess::ClearExpiredWithEnvelopeFailure(cache, diagnostic),
			"expired cleanup should fail when metadata replacement is injected to fail");
		Require(cache.Contains(uid), "failed expired cleanup must retain its live entry");
		Require(std::filesystem::exists(artifactPath),
			"failed expired cleanup must preserve artifacts until metadata commits");
		Require(ReadText(cachePath) == durableEnvelope,
			"failed expired cleanup must preserve the durable envelope");

		Require(PublishComplete(cache, uid, 1, 55),
			"same-size corruption should self-heal through replacement compilation");
		const std::string healedGeneration = ShaderCacheTestAccess::GetGeneration(cache, uid, 1);
		const auto healedArtifactPath = ShaderCacheTestAccess::GetArtifactPath(
			cache,
			uid,
			1,
			ShaderCache::VertexShaderTag,
			false);
		Require(healedGeneration != corruptGeneration && std::filesystem::exists(artifactPath),
			"self-heal should stage a new generation while retaining the durable corrupt generation");
		cache.SaveCache();
		Require(!cache.IsDirty() && ReadText(cachePath) != durableEnvelope,
			"self-heal should commit its replacement generation");
		Require(!std::filesystem::exists(artifactPath) && std::filesystem::exists(healedArtifactPath),
			"self-heal should sweep the corrupt generation only after replacement commit");
		TVector<uint32_t> vertex;
		TVector<uint32_t> fragment;
		TVector<uint32_t> compute;
		Require(cache.GetSpirvCode(uid, 1, vertex, fragment, compute),
			"self-healed bytecode should be readable");
		RequireWords(vertex, Words(55), "self-healed bytecode");

		const FileId cleanupUid = MakeFileId("{SHADER-CACHE-EXPIRED-CLEANUP}");
		Require(PublishComplete(cache, cleanupUid, 0, 60),
			"the successful expiry-cleanup fixture should publish");
		cache.SaveCache();
		const auto cleanupArtifactPath = ShaderCacheTestAccess::GetArtifactPath(
			cache,
			cleanupUid,
			0,
			ShaderCache::VertexShaderTag,
			false);
		WriteWords(cleanupArtifactPath, Words(600));
		ShaderCacheTestAccess::FailNextArtifactSweep(cache);
		cache.ClearExpired();
		Require(!cache.Contains(cleanupUid) && cache.IsDirty() &&
			std::filesystem::exists(cleanupArtifactPath),
			"expired cleanup should remain dirty when post-commit artifact sweeping fails");
		cache.SaveCache();
		Require(!cache.IsDirty() && !std::filesystem::exists(cleanupArtifactPath),
			"save retry should finish expired artifact sweeping after metadata commit");
		Require(cache.Contains(uid), "successful expired cleanup should retain unrelated healed metadata");
	}

	void TestIoFailureQuarantineIsReadOnlyAndSessionOnly()
	{
		TempDirectory directory;
		const std::filesystem::path cacheRoot = directory.Path("Cache");
		ShaderCache cache;
		Require(ShaderCacheTestAccess::Configure(cache, cacheRoot),
			"shader cache test storage should initialize");
		const FileId durableUid = MakeFileId("{SHADER-CACHE-QUARANTINE-DURABLE}");
		const FileId sessionUid = MakeFileId("{SHADER-CACHE-QUARANTINE-SESSION}");

		Require(PublishComplete(cache, durableUid, 0, 60), "the durable quarantine fixture should publish");
		cache.SaveCache(true);
		const auto cachePath = ShaderCacheTestAccess::GetCachePath(cache);
		const auto backupPath = cacheRoot / "ShaderCache.backup.yaml";
		std::filesystem::rename(cachePath, backupPath);
		std::filesystem::create_directory(cachePath);
		const std::string durableEnvelope = ReadText(backupPath);
		const size_t preservedFileCount = CountRegularFiles(cacheRoot);

		cache.LoadCache();
		Require(
			cache.GetLastLoadResult().m_status == Workspace::EWorkspaceCacheLoadStatus::IoFailure,
			"an unreadable non-file cache target should enter I/O quarantine");
		Require(ShaderCacheTestAccess::IsQuarantined(cache),
			"I/O failure should make shader cache storage read-only");
		Require(!cache.Contains(durableUid),
			"quarantine should withhold disk-backed metadata that could not be fully reloaded");

		Require(PublishComplete(cache, sessionUid, 2, 70),
			"compilation should remain available as memory-only data during quarantine");
		Require(cache.Contains(sessionUid), "memory-only quarantine publication should be visible in-session");
		Require(!cache.IsDirty(), "memory-only quarantine publication should not claim pending disk state");
		Require(CountRegularFiles(cacheRoot) == preservedFileCount,
			"memory-only quarantine publication must not create disk artifacts");

		TVector<uint32_t> vertex;
		TVector<uint32_t> fragment;
		TVector<uint32_t> compute;
		Require(cache.GetSpirvCode(sessionUid, 2, vertex, fragment, compute, false),
			"regular memory-only SPIR-V should be readable during quarantine");
		RequireWords(vertex, Words(70), "regular quarantine vertex SPIR-V");
		RequireWords(fragment, Words(71), "regular quarantine fragment SPIR-V");
		Require(compute.IsEmpty(), "regular quarantine graphics SPIR-V should not invent compute data");
		Require(cache.GetSpirvCode(sessionUid, 2, vertex, fragment, compute, true),
			"debug memory-only SPIR-V should be readable during quarantine");
		RequireWords(vertex, Words(72), "debug quarantine vertex SPIR-V");
		RequireWords(fragment, Words(73), "debug quarantine fragment SPIR-V");
		Require(compute.IsEmpty(), "debug quarantine graphics SPIR-V should not invent compute data");

		std::filesystem::remove(cachePath);
		cache.LoadCache();
		Require(
			cache.GetLastLoadResult().m_status == Workspace::EWorkspaceCacheLoadStatus::Missing,
			"a missing retry target should not escape an existing I/O quarantine");
		Require(ShaderCacheTestAccess::IsQuarantined(cache),
			"quarantine should persist until a fully successful reload or ClearAll");
		Require(cache.Contains(sessionUid),
			"a failed reload should retain session-only quarantine artifacts");

		cache.SaveCache(true);
		cache.ClearExpired();
		Require(ReadText(backupPath) == durableEnvelope,
			"save and cleanup must preserve unreadable durable storage during quarantine");
		Require(CountRegularFiles(cacheRoot) == preservedFileCount,
			"save and cleanup must not mutate artifact storage during quarantine");

		std::string writeDiagnostic;
		Require(
			Workspace::AtomicReplaceWorkspaceCacheText(
				cachePath,
				"not: [valid",
				writeDiagnostic),
			"a corrupt retry fixture should be writable: " + writeDiagnostic);
		cache.LoadCache();
		Require(
			cache.GetLastLoadResult().m_status == Workspace::EWorkspaceCacheLoadStatus::Corrupt,
			"a corrupt retry target should not escape an existing I/O quarantine");
		Require(ShaderCacheTestAccess::IsQuarantined(cache) && cache.Contains(sessionUid),
			"corrupt reload should retain read-only session state");
		cache.SaveCache(true);
		cache.ClearExpired();
		Require(ReadText(cachePath) == "not: [valid",
			"quarantine must not replace a corrupt retry target without ClearAll");

		std::filesystem::remove(cachePath);
		std::filesystem::rename(backupPath, cachePath);
		cache.LoadCache();
		Require(cache.GetLastLoadResult().IsLoaded(),
			"a later full successful reload should leave I/O quarantine");
		Require(!ShaderCacheTestAccess::IsQuarantined(cache),
			"successful full reload should restore persistent cache access");
		Require(cache.Contains(durableUid), "successful reload should restore durable metadata");
		Require(!cache.Contains(sessionUid),
			"successful reload should discard session-only quarantine artifacts");
	}

	void TestRuntimeArtifactIoFailureEntersReadOnlyQuarantine()
	{
		TempDirectory directory;
		const std::filesystem::path cacheRoot = directory.Path("Cache");
		ShaderCache cache;
		Require(ShaderCacheTestAccess::Configure(cache, cacheRoot),
			"shader cache test storage should initialize");
		const FileId uid = MakeFileId("{SHADER-CACHE-RUNTIME-IO-QUARANTINE}");

		Require(PublishComplete(cache, uid, 0, 80), "the runtime I/O fixture should publish");
		cache.SaveCache(true);
		const auto cachePath = ShaderCacheTestAccess::GetCachePath(cache);
		const auto artifactPath = ShaderCacheTestAccess::GetArtifactPath(
			cache,
			uid,
			0,
			ShaderCache::VertexShaderTag,
			false);
		const std::string durableEnvelope = ReadText(cachePath);
		const std::string durableArtifact = ReadText(artifactPath);
		const std::string durableGeneration = ShaderCacheTestAccess::GetGeneration(cache, uid, 0);
		const size_t durableFileCount = CountRegularFiles(cacheRoot);

		ShaderCacheTestAccess::SetArtifactReadIoFailure(cache, true);
		Require(cache.IsExpired(uid, 0),
			"runtime artifact I/O failure should force a session recompile");
		ShaderCacheTestAccess::SetArtifactReadIoFailure(cache, false);
		Require(ShaderCacheTestAccess::IsQuarantined(cache),
			"runtime artifact I/O failure should enter read-only storage quarantine");
		Require(
			cache.GetLastLoadResult().m_status == Workspace::EWorkspaceCacheLoadStatus::IoFailure,
			"runtime artifact I/O failure should publish an I/O load status");
		Require(!cache.Contains(uid),
			"runtime I/O quarantine should withhold the unread disk-backed entry");

		Require(PublishComplete(cache, uid, 0, 90),
			"runtime I/O quarantine should accept session-only recompiled bytecode");
		Require(cache.Contains(uid) && !cache.IsDirty(),
			"session-only recompile should be visible without pending disk mutation");
		TVector<uint32_t> vertex;
		TVector<uint32_t> fragment;
		TVector<uint32_t> compute;
		Require(cache.GetSpirvCode(uid, 0, vertex, fragment, compute),
			"session-only recompiled bytecode should be readable");
		RequireWords(vertex, Words(90), "runtime quarantine bytecode");

		cache.SaveCache(true);
		cache.ClearExpired();
		Require(ReadText(cachePath) == durableEnvelope,
			"runtime I/O quarantine must preserve durable metadata byte-for-byte");
		Require(ReadText(artifactPath) == durableArtifact,
			"runtime I/O quarantine must preserve durable artifacts byte-for-byte");
		Require(CountRegularFiles(cacheRoot) == durableFileCount,
			"runtime I/O quarantine must not create or collect disk files");

		cache.LoadCache();
		Require(cache.GetLastLoadResult().IsLoaded() &&
			!ShaderCacheTestAccess::IsQuarantined(cache),
			"a full successful reload should leave runtime I/O quarantine");
		Require(ShaderCacheTestAccess::GetGeneration(cache, uid, 0) == durableGeneration,
			"successful reload should restore the durable immutable generation");
		Require(cache.GetSpirvCode(uid, 0, vertex, fragment, compute),
			"restored durable bytecode should be readable");
		RequireWords(vertex, Words(80), "restored durable bytecode");
	}

	void TestShaderCompilerFailureLifecycle()
	{
		const bool allSucceeded[] = { true, true, true };
		const bool oneFailed[] = { true, false, true };
		Require(
			ShaderCompilerTestAccess::AggregateCompileResults(allSucceeded, std::size(allSucceeded)),
			"compile aggregation should retain all-success results");
		Require(
			!ShaderCompilerTestAccess::AggregateCompileResults(oneFailed, std::size(oneFailed)),
			"one failed permutation should fail the aggregate compile result");
		Require(ShaderCompilerTestAccess::ShouldRetryCacheSave(0, true),
			"current bytecode with dirty metadata should select the save-only retry branch");
		Require(!ShaderCompilerTestAccess::ShouldRetryCacheSave(0, false) &&
			!ShaderCompilerTestAccess::ShouldRetryCacheSave(1, true),
			"save-only retry should require both no compilation work and dirty metadata");
		Require(ShaderCompilerTestAccess::ExerciseFailedLoadEvictionAndRetry(),
			"failed shader load eviction should permit a second permutation load attempt");
		Require(ShaderCompilerTestAccess::ExercisePromiseGarbageCollection(),
			"promise garbage collection should retain newly pending permutations");

		TempDirectory directory;
		const std::filesystem::path cacheRoot = directory.Path("Cache");
		ShaderCache cache;
		Require(ShaderCacheTestAccess::Configure(cache, cacheRoot),
			"shader cache test storage should initialize");
		const FileId uid = MakeFileId("{SHADER-CACHE-SAVE-RETRY}");

		Require(PublishComplete(cache, uid, 0, 100), "the first save generation should publish");
		cache.SaveCache(true);
		const auto cachePath = ShaderCacheTestAccess::GetCachePath(cache);
		const auto oldArtifactPath = ShaderCacheTestAccess::GetArtifactPath(
			cache,
			uid,
			0,
			ShaderCache::VertexShaderTag,
			false);
		const std::string oldGeneration = ShaderCacheTestAccess::GetGeneration(cache, uid, 0);
		const std::string oldEnvelope = ReadText(cachePath);

		Require(PublishComplete(cache, uid, 0, 110), "the replacement generation should publish");
		const auto newArtifactPath = ShaderCacheTestAccess::GetArtifactPath(
			cache,
			uid,
			0,
			ShaderCache::VertexShaderTag,
			false);
		const std::string newGeneration = ShaderCacheTestAccess::GetGeneration(cache, uid, 0);
		Require(oldGeneration != newGeneration && cache.IsDirty(),
			"replacement publication should remain dirty under a new immutable generation");
		Require(std::filesystem::exists(oldArtifactPath) && std::filesystem::exists(newArtifactPath),
			"both generations should exist before metadata commit");

		ShaderCacheTestAccess::FailNextSaveBeforeReplace(cache);
		Require(
			!ShaderCompilerTestAccess::SaveCacheAndCombineResult(cache, true),
			"compile orchestration should report the first injected save failure");
		Require(cache.IsDirty() && ReadText(cachePath) == oldEnvelope,
			"failed save should retain dirty state and the old durable envelope");
		Require(std::filesystem::exists(oldArtifactPath) && std::filesystem::exists(newArtifactPath),
			"failed save must retain both the durable and uncommitted generations");

		Require(ShaderCompilerTestAccess::SaveCacheAndCombineResult(cache, true),
			"a no-recompile save retry should publish the already compiled generation");
		Require(!cache.IsDirty() && ReadText(cachePath) != oldEnvelope,
			"successful retry should commit the new envelope and clear dirty state");
		Require(!std::filesystem::exists(oldArtifactPath) && std::filesystem::exists(newArtifactPath),
			"post-commit GC should remove only the old immutable generation");
		Require(!ShaderCompilerTestAccess::SaveCacheAndCombineResult(cache, false),
			"a compile failure should remain failed even when cache persistence succeeds");
	}

	void TestShaderSourceNormalization()
	{
		std::string diagnostic;
		std::string rawGlsl =
			"vec3 convertRGB2XYZ(vec3 value)\n"
			"{\n"
			"    // Reference(s):\n"
			"\treturn value;\n"
			"}\n";
		const std::string originalRawGlsl = rawGlsl;
		Require(
			!ShaderCompilerTestAccess::NormalizeShaderTabs("glsl", rawGlsl, diagnostic),
			"raw GLSL should not be normalized as shader YAML");
		Require(rawGlsl == originalRawGlsl && diagnostic.empty(),
			"raw GLSL should remain byte-for-byte unchanged without a YAML diagnostic");

		std::string validShader =
			"glslVertex: |\n"
			"  void main() {}\n";
		const std::string originalValidShader = validShader;
		Require(
			!ShaderCompilerTestAccess::NormalizeShaderTabs("shader", validShader, diagnostic),
			"valid shader YAML should not need normalization");
		Require(validShader == originalValidShader && diagnostic.empty(),
			"valid shader YAML should remain byte-for-byte unchanged");

		std::string shaderWithTab =
			"glslVertex: |\n"
			"  void main()\n"
			"  {\n"
			"\tgl_Position = vec4(0.0);\n"
			"  }\n";
		Require(
			ShaderCompilerTestAccess::NormalizeShaderTabs("shader", shaderWithTab, diagnostic),
			"invalid YAML indentation tabs should be normalized");
		Require(shaderWithTab.find('\t') == std::string::npos && diagnostic.empty(),
			"normalized shader YAML should contain no tabs or diagnostic");
		Require(shaderWithTab.find("\n    gl_Position") != std::string::npos,
			"shader YAML tabs should expand to exactly four spaces");
		Require(YAML::Load(shaderWithTab)["glslVertex"].IsScalar(),
			"normalized shader YAML should be parseable");

		std::string malformedShader = "glslVertex: [\n";
		const std::string originalMalformedShader = malformedShader;
		Require(
			!ShaderCompilerTestAccess::NormalizeShaderTabs("shader", malformedShader, diagnostic),
			"non-tab YAML errors should not trigger a rewrite");
		Require(malformedShader == originalMalformedShader && !diagnostic.empty(),
			"non-tab YAML errors should preserve the source and publish a diagnostic");
	}

	void TestShaderSourceRewritePreservesFileIdentity()
	{
		TempDirectory directory;
		const std::filesystem::path shaderPath = directory.Path("User.shader");
		const std::filesystem::path hardLinkPath = directory.Path("User.shader.link");
		const std::string onDiskSource =
			"glslVertex: |\r\n"
			"  void main()\n"
			"  {\r\n"
			"\tgl_Position = vec4(0.0);\n"
			"  }\r\n";
		{
			std::ofstream output(shaderPath, std::ios::binary);
			Require(output.is_open(), "the shader rewrite fixture should be writable");
			output << onDiskSource;
		}

		std::string originalSource;
		std::string diagnostic;
		Require(
			ShaderCompilerTestAccess::ReadShaderSourceBinary(
				shaderPath.generic_string(),
				originalSource,
				diagnostic),
			"the shader rewrite fixture should use production binary reading: " + diagnostic);
		Require(originalSource == onDiskSource,
			"production shader reading should preserve mixed line endings byte-for-byte");
		std::string normalizedSource = originalSource;
		Require(
			ShaderCompilerTestAccess::NormalizeShaderTabs(
				"shader",
				normalizedSource,
				diagnostic),
			"the rewrite fixture should require normalization");
		std::error_code filesystemError;
		std::filesystem::create_hard_link(shaderPath, hardLinkPath, filesystemError);
		Require(!filesystemError,
			"the shader rewrite fixture should support hard links: " + filesystemError.message());
		const auto permissionsBefore = std::filesystem::status(shaderPath).permissions();

		Require(
			ShaderCompilerTestAccess::RewriteShaderSourceInPlace(
				shaderPath.generic_string(),
				originalSource,
				normalizedSource,
				diagnostic),
			"normalized shader source should be written in place: " + diagnostic);
		Require(ReadText(shaderPath) == normalizedSource && ReadText(hardLinkPath) == normalizedSource,
			"in-place normalization should preserve the source inode");
		Require(std::filesystem::status(shaderPath).permissions() == permissionsBefore,
			"in-place normalization should preserve source permissions");
		Require(CountRegularFiles(shaderPath.parent_path()) == 2,
			"in-place normalization should not create discoverable temporary assets");

		const std::string onDiskConcurrentSource = "glslVertex: concurrent-edit\r\n";
		{
			std::ofstream output(shaderPath, std::ios::binary | std::ios::trunc);
			Require(output.is_open(), "the concurrent shader edit should be writable");
			output << onDiskConcurrentSource;
		}
		const std::string concurrentSource = onDiskConcurrentSource;
		Require(
			!ShaderCompilerTestAccess::RewriteShaderSourceInPlace(
				shaderPath.generic_string(),
				originalSource,
				normalizedSource,
				diagnostic),
			"a concurrent user edit should cancel shader normalization");
		Require(ReadText(shaderPath) == concurrentSource && !diagnostic.empty(),
			"a concurrent user edit should remain intact and produce a diagnostic");
	}
}

int main()
{
	try
	{
		TestArtifactRoundTrip();
		TestTruncatedArtifactIsRejected();
		TestMisalignedArtifactIsRejected();
		TestChecksumMismatchIsRejected();
		TestOwnedArtifactContainment();
		TestDebugArtifactsAreRequired();
		TestFailedGenerationPreservesDurableGeneration();
		TestRemoveCommitsBeforeGarbageCollection();
		TestSameSizeChecksumCorruptionAndClearExpiredTransaction();
		TestIoFailureQuarantineIsReadOnlyAndSessionOnly();
		TestRuntimeArtifactIoFailureEntersReadOnlyQuarantine();
		TestShaderCompilerFailureLifecycle();
		TestShaderSourceNormalization();
		TestShaderSourceRewritePreservesFileIdentity();
		std::cout << "Shader cache artifact tests passed.\n";
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::cerr << "Shader cache artifact tests failed: " << exception.what() << '\n';
		return 1;
	}
}
