#include "AssetRegistry/AssetCache.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <yaml-cpp/yaml.h>

namespace
{
	using namespace Sailor;

	class TestAssetCache final : public AssetCache
	{
	public:
		using CacheData = AssetCacheData;
		using CacheEntry = AssetCacheData::Entry;
		using AssetCache::SerializeAssetCachePayload;
		using AssetCache::ShouldResetCacheFile;
		using AssetCache::ShouldWriteCacheFile;
		using AssetCache::TryDeserializeAssetCachePayload;
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

	TestAssetCache::CacheData MakeCache(
		const std::string& fileIdValue,
		const std::string& sourcePath,
		std::time_t timestamp)
	{
		TestAssetCache::CacheData result;
		const FileId fileId = MakeFileId(fileIdValue);
		TestAssetCache::CacheEntry entry;
		entry.m_fileId = fileId;
		entry.m_sourcePath = sourcePath;
		entry.m_assetImportTime = timestamp;
		result.m_data.Insert(fileId, std::move(entry));
		return result;
	}

	void TestPayloadRoundTrip()
	{
		const FileId fileId = MakeFileId("{ASSET-CACHE-ROUNDTRIP}");
		const auto source = MakeCache(fileId.ToString(), "/workspace/Content/Test.mat", 42);
		const std::string payload = TestAssetCache::SerializeAssetCachePayload(source);

		TestAssetCache::CacheData loaded;
		std::string diagnostic;
		Require(
			TestAssetCache::TryDeserializeAssetCachePayload(payload, loaded, diagnostic),
			"current asset payload should load: " + diagnostic);
		Require(loaded.m_data.ContainsKey(fileId), "round-tripped payload should retain its file id");
		const auto entry = loaded.m_data[fileId];
		Require(entry.m_fileId == fileId, "round-tripped entry identity should match its map key");
		Require(entry.m_assetImportTime == 42, "round-tripped timestamp should be preserved");
		Require(!entry.m_sourcePath.empty(), "round-tripped source path should be normalized");
	}

	void TestEmptyPayloadRoundTrip()
	{
		const TestAssetCache::CacheData source;
		const std::string payload = TestAssetCache::SerializeAssetCachePayload(source);

		TestAssetCache::CacheData loaded;
		std::string diagnostic;
		Require(
			TestAssetCache::TryDeserializeAssetCachePayload(payload, loaded, diagnostic),
			"deterministic empty asset payload should load: " + diagnostic);
		Require(loaded.m_data.Num() == 0, "empty asset payload should remain empty");
	}

	void TestCorruptPayloadDoesNotPartiallyPublish()
	{
		const FileId retainedId = MakeFileId("{ASSET-CACHE-RETAINED}");
		auto destination = MakeCache(retainedId.ToString(), "/workspace/Content/Retained.mat", 7);

		const std::string corruptPayload =
			"assetCache:\n"
			"  assets:\n"
			"    '{ASSET-CACHE-CORRUPT}':\n"
			"      fileId: '{ASSET-CACHE-CORRUPT}'\n"
			"      assetImportTime: 8\n"
			"      sourcePath: ''\n";
		std::string diagnostic;
		Require(
			!TestAssetCache::TryDeserializeAssetCachePayload(corruptPayload, destination, diagnostic),
			"empty sourcePath should reject the complete payload");
		Require(!diagnostic.empty(), "corrupt asset payload should return an actionable diagnostic");
		Require(destination.m_data.Num() == 1 && destination.m_data.ContainsKey(retainedId),
			"failed payload validation must not partially replace existing state");
	}

	void TestMismatchedEntryIdentityIsCorrupt()
	{
		const std::string payload =
			"assetCache:\n"
			"  assets:\n"
			"    '{ASSET-CACHE-KEY}':\n"
			"      fileId: '{ASSET-CACHE-OTHER}'\n"
			"      assetImportTime: 9\n"
			"      sourcePath: '/workspace/Content/Test.mat'\n";

		TestAssetCache::CacheData destination;
		std::string diagnostic;
		Require(
			!TestAssetCache::TryDeserializeAssetCachePayload(payload, destination, diagnostic),
			"an entry whose fileId disagrees with its map key should be rejected");
		Require(diagnostic.find("mismatched") != std::string::npos,
			"identity mismatch diagnostic should explain the corruption");
	}

	void TestDirectDeserializeDoesNotThrowOnCorruptData()
	{
		TestAssetCache::CacheData destination = MakeCache(
			"{ASSET-CACHE-PRESERVE-ON-THROW}",
			"/workspace/Content/Preserved.mat",
			99);

		const std::string corruptPayload =
			"assetCache:\n"
			"  assets:\n"
			"    '{ASSET-CACHE-THROW}':\n"
			"      fileId: '{ASSET-CACHE-THROW}'\n"
			"      assetImportTime: abc\n"
			"      sourcePath: '/workspace/Content/Test.mat'\n";
		const YAML::Node node = YAML::Load(corruptPayload)["assetCache"];

		try
		{
			destination.Deserialize(node);
		}
		catch (const std::exception& exception)
		{
			throw std::runtime_error("Asset cache direct Deserialize should never throw: " + std::string(exception.what()));
		}
		catch (...)
		{
			throw std::runtime_error("Asset cache direct Deserialize should never throw.");
		}

		Require(destination.m_data.Num() == 0, "corrupt direct deserialize payload should not leave stale data");
	}

	void TestEntryDeserializeDoesNotThrow()
	{
		TestAssetCache::CacheEntry entry{};
		const YAML::Node invalidEntry = YAML::Load(
			"fileId:\n"
			"  list: value\n"
			"assetImportTime: invalid\n"
			"sourcePath: ''");

		try
		{
			entry.Deserialize(invalidEntry);
		}
		catch (const std::exception& exception)
		{
			throw std::runtime_error("Asset cache entry Deserialize should never throw: " + std::string(exception.what()));
		}
		catch (...)
		{
			throw std::runtime_error("Asset cache entry Deserialize should never throw.");
		}
	}

	void TestIoFailurePreservesTheExistingCacheFile()
	{
		using Status = Workspace::EWorkspaceCacheLoadStatus;
		Require(TestAssetCache::ShouldResetCacheFile(Status::Missing),
			"a missing cache should be initialized with a current envelope");
		Require(TestAssetCache::ShouldResetCacheFile(Status::StaleIdentity),
			"a stale cache should be invalidated");
		Require(TestAssetCache::ShouldResetCacheFile(Status::Corrupt),
			"a corrupt cache should be invalidated");
		Require(TestAssetCache::ShouldResetCacheFile(Status::UnsupportedVersion),
			"an unsupported cache should be invalidated");
		Require(!TestAssetCache::ShouldResetCacheFile(Status::IoFailure),
			"a transient I/O failure must preserve the existing cache file");
		Require(!TestAssetCache::ShouldResetCacheFile(Status::Loaded),
			"a loaded cache must not be reset");
		Require(!TestAssetCache::ShouldWriteCacheFile(false, false, true),
			"idle shutdown after an I/O failure must preserve the existing cache file");
		Require(!TestAssetCache::ShouldWriteCacheFile(false, true, true),
			"ordinary updates after an I/O failure must not authorize replacing unreadable storage");
		Require(TestAssetCache::ShouldWriteCacheFile(true, true, true),
			"an explicit forced rebuild may authorize replacing preserved cache storage");
		Require(TestAssetCache::ShouldWriteCacheFile(false, true, false),
			"a dirty cache without an I/O preservation barrier should be persisted");
	}
}

int main()
{
	try
	{
		TestPayloadRoundTrip();
		TestEmptyPayloadRoundTrip();
		TestCorruptPayloadDoesNotPartiallyPublish();
		TestMismatchedEntryIdentityIsCorrupt();
		TestDirectDeserializeDoesNotThrowOnCorruptData();
		TestEntryDeserializeDoesNotThrow();
		TestIoFailurePreservesTheExistingCacheFile();
		std::cout << "Asset cache contract tests passed.\n";
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::cerr << "Asset cache contract tests failed: " << exception.what() << '\n';
		return 1;
	}
}
