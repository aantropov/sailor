#include "AssetRegistry/AssetCache.h"
#include "AssetRegistry/AssetInfo.h"

#include <chrono>
#include <filesystem>
#include <fstream>
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
		using AssetCache::Update;
	};

	class TempDirectory final
	{
	public:
		explicit TempDirectory(const char* label)
		{
			static uint64_t counter = 0;
			m_path = std::filesystem::temp_directory_path() /
				("sailor-asset-cache-" + std::string(label) + "-" + std::to_string(++counter));
			std::filesystem::remove_all(m_path);
			std::filesystem::create_directories(m_path);
		}

		~TempDirectory()
		{
			std::error_code error;
			std::filesystem::remove_all(m_path, error);
		}

		std::filesystem::path Path(const std::filesystem::path& relative) const
		{
			return m_path / relative;
		}

	private:
		std::filesystem::path m_path;
	};

	class TestAssetInfo final : public AssetInfo
	{
	public:
		void Configure(
			const FileId& fileId,
			const std::filesystem::path& sourcePath,
			const std::filesystem::path& metadataPath)
		{
			m_fileId = fileId;
			m_folder = sourcePath.parent_path().string() + std::filesystem::path::preferred_separator;
			m_assetFilename = sourcePath.filename().string();
			m_metaFilepath = metadataPath.string();
		}
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
		TestAssetCache::FileTimestamp sourceTimestamp,
		const std::string& metadataPath,
		TestAssetCache::FileTimestamp metadataTimestamp)
	{
		TestAssetCache::CacheData result;
		const FileId fileId = MakeFileId(fileIdValue);
		TestAssetCache::CacheEntry entry;
		entry.m_fileId = fileId;
		entry.m_sourcePath = sourcePath;
		entry.m_sourceTimestamp = sourceTimestamp;
		entry.m_metadataPath = metadataPath;
		entry.m_metadataTimestamp = metadataTimestamp;
		result.m_data.Insert(fileId, std::move(entry));
		return result;
	}

	void WriteFile(const std::filesystem::path& path, const std::string& content)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream stream(path, std::ios::binary);
		stream << content;
	}

	void TestPayloadRoundTrip()
	{
		const FileId fileId = MakeFileId("{ASSET-CACHE-ROUNDTRIP}");
		const auto source = MakeCache(
			fileId.ToString(),
			"/workspace/Content/Test.mat",
			42,
			"/workspace/Content/Test.mat.asset",
			84);
		const std::string payload = TestAssetCache::SerializeAssetCachePayload(source);

		TestAssetCache::CacheData loaded;
		std::string diagnostic;
		Require(
			TestAssetCache::TryDeserializeAssetCachePayload(payload, loaded, diagnostic),
			"current asset payload should load: " + diagnostic);
		Require(loaded.m_data.ContainsKey(fileId), "round-tripped payload should retain its file id");
		const auto entry = loaded.m_data[fileId];
		Require(entry.m_fileId == fileId, "round-tripped entry identity should match its map key");
		Require(entry.m_sourceTimestamp == 42, "round-tripped source timestamp should be preserved");
		Require(!entry.m_sourcePath.empty(), "round-tripped source path should be normalized");
		Require(entry.m_metadataTimestamp == 84, "round-tripped metadata timestamp should be preserved");
		Require(!entry.m_metadataPath.empty(), "round-tripped metadata path should be normalized");
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
		auto destination = MakeCache(
			retainedId.ToString(),
			"/workspace/Content/Retained.mat",
			7,
			"/workspace/Content/Retained.mat.asset",
			8);

		const std::string corruptPayload =
			"assetCache:\n"
			"  assets:\n"
			"    '{ASSET-CACHE-CORRUPT}':\n"
			"      fileId: '{ASSET-CACHE-CORRUPT}'\n"
			"      sourceTimestamp: 8\n"
			"      sourcePath: '/workspace/Content/Test.mat'\n"
			"      metadataTimestamp: 9\n"
			"      metadataPath: ''\n";
		std::string diagnostic;
		Require(
			!TestAssetCache::TryDeserializeAssetCachePayload(corruptPayload, destination, diagnostic),
			"empty metadataPath should reject the complete payload");
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
			"      sourceTimestamp: 9\n"
			"      sourcePath: '/workspace/Content/Test.mat'\n"
			"      metadataTimestamp: 10\n"
			"      metadataPath: '/workspace/Content/Test.mat.asset'\n";

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
			99,
			"/workspace/Content/Preserved.mat.asset",
			100);

		const std::string corruptPayload =
			"assetCache:\n"
			"  assets:\n"
			"    '{ASSET-CACHE-THROW}':\n"
			"      fileId: '{ASSET-CACHE-THROW}'\n"
			"      sourceTimestamp: abc\n"
			"      sourcePath: '/workspace/Content/Test.mat'\n"
			"      metadataTimestamp: 10\n"
			"      metadataPath: '/workspace/Content/Test.mat.asset'\n";
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
			"sourceTimestamp: invalid\n"
			"sourcePath: ''\n"
			"metadataTimestamp: invalid\n"
			"metadataPath: ''");

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

	void TestUpdateTracksBothObservationsAndPreservesDirtyState()
	{
		TestAssetCache cache;
		const FileId fileId = MakeFileId("{ASSET-CACHE-UPDATE}");
		const std::string sourcePath = "/workspace/Content/Test.mat";
		const std::string metadataPath = sourcePath + ".asset";

		Require(cache.Update(fileId, sourcePath, 100, metadataPath, 200),
			"a new source/metadata observation should change the cache");
		Require(cache.IsDirty(), "a new observation should mark the cache dirty");
		Require(!cache.Update(fileId, sourcePath, 100, metadataPath, 200),
			"an unchanged source/metadata observation should be a no-op");
		Require(cache.IsDirty(), "a no-op update must not clear an already dirty cache");
		Require(cache.Update(fileId, sourcePath, 99, metadataPath, 200),
			"a backdated source timestamp should change the cache");
		Require(cache.Update(fileId, sourcePath, 99, metadataPath, 199),
			"a backdated metadata timestamp should change the cache");
		Require(cache.Update(fileId, sourcePath + ".moved", 99, metadataPath, 199),
			"a source path change should change the cache");
		Require(cache.Update(fileId, sourcePath + ".moved", 99, metadataPath + ".moved", 199),
			"a metadata path change should change the cache");
	}

	void TestFilesystemObservationsExpireOnEitherFile()
	{
		TempDirectory directory("observations");
		const std::filesystem::path sourcePath = directory.Path("Content/Test.mat");
		const std::filesystem::path metadataPath = directory.Path("Content/Test.mat.asset");
		WriteFile(sourcePath, "source");
		WriteFile(metadataPath, "metadata");

		using FileDuration = std::filesystem::file_time_type::duration;
		const auto initialDuration = std::chrono::duration_cast<std::chrono::seconds>(
			std::filesystem::file_time_type::clock::now().time_since_epoch()) - std::chrono::seconds(10);
		const std::filesystem::file_time_type initialTime(
			std::chrono::duration_cast<FileDuration>(initialDuration));
		std::filesystem::last_write_time(sourcePath, initialTime);
		std::filesystem::last_write_time(metadataPath, initialTime);

		TestAssetInfo info;
		info.Configure(MakeFileId("{ASSET-CACHE-FILESYSTEM}"), sourcePath, metadataPath);
		TestAssetCache cache;
		Require(cache.Update(&info), "the initial filesystem observation should populate the cache");
		Require(!cache.IsExpired(&info), "unchanged source and metadata files should remain current");

		const FileDuration subsecondDelta = std::chrono::duration_cast<FileDuration>(
			std::chrono::milliseconds(100));
		Require(subsecondDelta.count() != 0,
			"the filesystem clock should represent sub-second cache observations");
		std::filesystem::last_write_time(sourcePath, initialTime + subsecondDelta);
		Require(cache.IsExpired(&info),
			"a source change within the same second should expire the cache");
		Require(cache.Update(&info), "the sub-second source timestamp should be recorded");
		Require(!cache.IsExpired(&info), "the refreshed sub-second observation should be current");

		std::filesystem::last_write_time(sourcePath, initialTime - std::chrono::seconds(1));
		Require(cache.IsExpired(&info), "a backdated source file should expire the cache");
		Require(cache.Update(&info), "the changed source timestamp should be recorded");
		Require(!cache.IsExpired(&info), "the refreshed source observation should be current");

		std::filesystem::last_write_time(metadataPath, initialTime - std::chrono::seconds(2));
		Require(cache.IsExpired(&info), "a backdated metadata file should expire the cache");
		Require(cache.Update(&info), "the changed metadata timestamp should be recorded");
		Require(!cache.IsExpired(&info), "the refreshed metadata observation should be current");

		std::filesystem::remove(metadataPath);
		Require(cache.IsExpired(&info), "a missing metadata file should always expire the cache");
		Require(!cache.Update(&info), "a missing file must not establish a clean cache observation");
		Require(cache.IsExpired(&info), "the cache must remain expired while metadata is missing");
	}

	void TestMetadataFieldsAreStrictAndTransactional()
	{
		const FileId retainedId = MakeFileId("{ASSET-CACHE-STRICT-RETAINED}");
		const std::string prefix =
			"assetCache:\n"
			"  assets:\n"
			"    '{ASSET-CACHE-STRICT}':\n"
			"      fileId: '{ASSET-CACHE-STRICT}'\n"
			"      sourceTimestamp: 1\n"
			"      sourcePath: '/workspace/Content/Test.mat'\n";
		const std::string invalidPayloads[] =
		{
			prefix + "      metadataTimestamp: 2\n",
			prefix +
				"      metadataTimestamp: 2\n"
				"      metadataPath: '/workspace/Content/Test.mat.asset'\n"
				"      metadataPath: '/workspace/Content/Duplicate.asset'\n",
			prefix +
				"      metadataTimestamp:\n"
				"        - 2\n"
				"      metadataPath:\n"
				"        nested: value\n"
		};

		for (const std::string& payload : invalidPayloads)
		{
			auto destination = MakeCache(
				retainedId.ToString(),
				"/workspace/Content/Retained.mat",
				7,
				"/workspace/Content/Retained.mat.asset",
				8);
			std::string diagnostic;
			Require(!TestAssetCache::TryDeserializeAssetCachePayload(payload, destination, diagnostic),
				"missing, duplicate, or non-scalar metadata fields should reject the payload");
			Require(!diagnostic.empty(), "strict metadata validation should return a diagnostic");
			Require(destination.m_data.Num() == 1 && destination.m_data.ContainsKey(retainedId),
				"strict metadata validation failures must not partially publish state");
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
		TestUpdateTracksBothObservationsAndPreservesDirtyState();
		TestFilesystemObservationsExpireOnEitherFile();
		TestMetadataFieldsAreStrictAndTransactional();
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
