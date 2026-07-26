#include "AssetRegistry/AssetCache.h"
#include "AssetRegistry/Animation/AnimationAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/FrameGraph/FrameGraphAssetInfo.h"
#include "AssetRegistry/Material/MaterialAssetInfo.h"
#include "AssetRegistry/Model/GeneratedModelAssetMetadata.h"
#include "AssetRegistry/Model/ModelAssetInfo.h"
#include "AssetRegistry/Prefab/PrefabAssetInfo.h"
#include "AssetRegistry/Shader/ShaderAssetInfo.h"
#include "AssetRegistry/Texture/TextureAssetInfo.h"
#include "AssetRegistry/World/WorldPrefabAssetInfo.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
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
		using AssetCache::Prune;
		using AssetCache::ShouldResetCacheFile;
		using AssetCache::ShouldWriteCacheFile;
		using AssetCache::TryDeserializeAssetCachePayload;
		using AssetCache::Update;
		using AssetCache::RestoreAssetImportTime;
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
			m_folder = sourcePath.parent_path().generic_string() + '/';
			m_assetFilename = sourcePath.filename().string();
			m_metaFilepath = metadataPath.string();
		}

		void SetProcessingTimes(std::time_t assetImportTime, std::time_t metadataLoadTime)
		{
			m_assetImportTime = assetImportTime;
			m_metaLoadTime = metadataLoadTime;
			Utils::TryGetFileRevision(GetAssetFilepath(), m_importedSourceRevision);
			Utils::TryGetFileRevision(GetMetaFilepath(), m_metadataRevision);
		}

		std::time_t GetRuntimeMetadataLoadTime() const
		{
			return m_metaLoadTime;
		}

		void SetPendingUpdate(bool bWasExpired)
		{
			m_bPendingUpdateNotification = true;
			m_bPendingWasExpired = bWasExpired;
		}

		void SaveMetaFile() override
		{
			++m_numMetaSaves;
		}

		YAML::Node Serialize() const override
		{
			YAML::Node result(YAML::NodeType::Map);
			result["fileId"] = m_fileId;
			result["testValue"] = m_testValue;
			result["lateValue"] = 1;
			return result;
		}

		void Deserialize(const YAML::Node& inData) override
		{
			m_fileId = inData["fileId"].as<FileId>();
			m_testValue = inData["testValue"].as<int32_t>();
			(void)inData["lateValue"].as<int32_t>();
			std::function<void()> onDeserialize = std::move(m_onDeserialize);
			if (onDeserialize)
			{
				onDeserialize();
			}
		}

		uint32_t m_numMetaSaves = 0;
		int32_t m_testValue = 0;
		std::function<void()> m_onDeserialize;
	};

	class RecordingAssetListener final : public IAssetInfoHandlerListener
	{
	public:
		void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override
		{
			m_events.emplace_back(bWasExpired ? "update:true" : "update:false");
			if (bWasExpired && m_onExpiredUpdate)
			{
				m_onExpiredUpdate(assetInfo);
			}
		}

		void OnImportAsset(AssetInfoPtr) override
		{
			m_events.emplace_back("import");
		}

		std::vector<std::string> m_events;
		std::function<void(AssetInfoPtr)> m_onExpiredUpdate;
	};

	class TestAssetInfoHandler final : public IAssetInfoHandler
	{
	public:
		void GetDefaultMeta(YAML::Node& outDefaultYaml) const override
		{
			outDefaultYaml = YAML::Node(YAML::NodeType::Map);
		}

		AssetInfoPtr LoadAssetInfo(
			const std::string& metaFilepath,
			const std::string&,
			EAssetMountKind,
			bool,
			bool,
			bool) const override
		{
			auto* info = new TestAssetInfo();
			const std::filesystem::path metadataPath(metaFilepath);
			std::filesystem::path sourcePath(metaFilepath);
			sourcePath.replace_extension();
			info->Configure(MakeTestFileId(), sourcePath, metadataPath);
			info->SetPendingUpdate(true);
			return info;
		}

	protected:
		AssetInfoPtr CreateAssetInfo() const override
		{
			return new TestAssetInfo();
		}

	private:
		static FileId MakeTestFileId()
		{
			FileId result;
			result.Deserialize(YAML::Node("{ASSET-CACHE-IMPORT-CONTRACT}"));
			return result;
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

	FileRevision MakeRevision(
		int64_t modificationTimeNanoseconds = 123456789,
		uint64_t fileSize = 64,
		uint64_t contentHash = 14695981039346656037ull)
	{
		FileRevision result;
		result.m_modificationTimeNanoseconds = modificationTimeNanoseconds;
		result.m_fileSize = fileSize;
		result.m_contentHash = contentHash;
		result.m_bIsValid = true;
		return result;
	}

	template<typename TAssetInfo>
	void RequireSerializedAssetInfoType(const std::string& expectedType)
	{
		const TAssetInfo assetInfo;
		const YAML::Node metadata = assetInfo.Serialize();
		Require(metadata.IsMap(), expectedType + " metadata must serialize as a map");
		Require(metadata["assetInfoType"].IsScalar(),
			expectedType + " metadata must contain a scalar assetInfoType");
		Require(metadata["assetInfoType"].as<std::string>() == expectedType,
			expectedType + " metadata must preserve its concrete reflected type");
	}

	void TestEveryAssetInfoSerializerWritesItsConcreteType()
	{
		RequireSerializedAssetInfoType<AssetInfo>("Sailor::AssetInfo");
		RequireSerializedAssetInfoType<AnimationAssetInfo>("Sailor::AnimationAssetInfo");
		RequireSerializedAssetInfoType<FrameGraphAssetInfo>("Sailor::FrameGraphAssetInfo");
		RequireSerializedAssetInfoType<MaterialAssetInfo>("Sailor::MaterialAssetInfo");
		RequireSerializedAssetInfoType<ModelAssetInfo>("Sailor::ModelAssetInfo");
		RequireSerializedAssetInfoType<PrefabAssetInfo>("Sailor::PrefabAssetInfo");
		RequireSerializedAssetInfoType<ShaderAssetInfo>("Sailor::ShaderAssetInfo");
		RequireSerializedAssetInfoType<TextureAssetInfo>("Sailor::TextureAssetInfo");
		RequireSerializedAssetInfoType<WorldPrefabAssetInfo>("Sailor::WorldPrefabAssetInfo");
	}

	void TestGeneratedGlbMetadataUsesTypedDefaults()
	{
		const FileId textureId = MakeFileId("{GENERATED-GLB-TEXTURE}");
		const YAML::Node texture = GeneratedModelAssetMetadata::CreateTexture(
			textureId,
			"Character.glb",
			7,
			false,
			RHI::ETextureFormat::R8G8B8A8_UNORM,
			RHI::ETextureClamping::Clamp,
			RHI::ETextureFiltration::Nearest,
			true);
		Require(texture["assetInfoType"].as<std::string>() == "Sailor::TextureAssetInfo",
			"generated GLB textures must declare TextureAssetInfo");
		Require(texture["fileId"].as<FileId>() == textureId,
			"generated GLB textures must preserve their generated FileId");
		Require(texture["filename"].as<std::string>() == "Character.glb" &&
			texture["glbTextureIndex"].as<uint32_t>() == 7,
			"generated GLB textures must reference the source model and embedded texture");
		Require(!texture["bShouldGenerateMips"].as<bool>() &&
			texture["bShouldKeepCpuBuffers"].as<bool>(),
			"generated GLB textures must preserve importer options");
		Require(texture["bShouldSupportStorageBinding"].IsDefined() &&
			texture["reduction"].IsDefined(),
			"generated GLB textures must retain every typed default field");

		const FileId animationId = MakeFileId("{GENERATED-GLB-ANIMATION}");
		const YAML::Node animation = GeneratedModelAssetMetadata::CreateAnimation(
			animationId,
			"Character.glb",
			3,
			2);
		Require(animation["assetInfoType"].as<std::string>() == "Sailor::AnimationAssetInfo",
			"generated GLB animations must declare AnimationAssetInfo");
		Require(animation["fileId"].as<FileId>() == animationId,
			"generated GLB animations must preserve their generated FileId");
		Require(animation["filename"].as<std::string>() == "Character.glb" &&
			animation["animationIndex"].as<uint32_t>() == 3 &&
			animation["skinIndex"].as<uint32_t>() == 2,
			"generated GLB animations must preserve every generated field");
	}

	TestAssetCache::CacheData MakeCache(
		const std::string& fileIdValue,
		const std::string& sourcePath,
		std::time_t assetImportTime)
	{
		TestAssetCache::CacheData result;
		const FileId fileId = MakeFileId(fileIdValue);
		TestAssetCache::CacheEntry entry;
		entry.m_fileId = fileId;
		entry.m_assetImportTime = assetImportTime;
		entry.m_sourcePath = sourcePath;
		entry.m_sourceRevision = MakeRevision();
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
			40);
		const std::string payload = TestAssetCache::SerializeAssetCachePayload(source);
		const YAML::Node serializedEntry = YAML::Load(payload)["assetCache"]["assets"][fileId.ToString()];
		Require(serializedEntry.IsMap() && serializedEntry.size() == 4,
			"persisted asset cache entries must contain exactly four fields");
		const YAML::Node serializedRevision = serializedEntry["sourceRevision"];
		Require(serializedRevision.IsMap() && serializedRevision.size() == 3,
			"persisted source revisions must contain mtime, size, and content hash");
		Require(payload.find("metadataLoadTime") == std::string::npos &&
			payload.find("metadataPath") == std::string::npos,
			"runtime metadata state must never be serialized into the asset cache");

		TestAssetCache::CacheData loaded;
		std::string diagnostic;
		Require(
			TestAssetCache::TryDeserializeAssetCachePayload(payload, loaded, diagnostic),
			"current asset payload should load: " + diagnostic);
		Require(loaded.m_data.ContainsKey(fileId), "round-tripped payload should retain its file id");
		const auto entry = loaded.m_data[fileId];
		Require(entry.m_fileId == fileId, "round-tripped entry identity should match its map key");
		Require(entry.m_assetImportTime == 40, "round-tripped asset import time should be preserved");
		Require(!entry.m_sourcePath.empty(), "round-tripped source path should be normalized");
		Require(entry.m_sourceRevision == MakeRevision(),
			"round-tripped exact source revision should be preserved");
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

	void TestLegacyTimestampOnlyPayloadIsRejected()
	{
		const std::string legacyPayload =
			"assetCache:\n"
			"  assets:\n"
			"    '{ASSET-CACHE-V1}':\n"
			"      fileId: '{ASSET-CACHE-V1}'\n"
			"      assetImportTime: 7\n"
			"      sourcePath: '/workspace/Content/Test.mat'\n";
		TestAssetCache::CacheData destination;
		std::string diagnostic;
		Require(!TestAssetCache::TryDeserializeAssetCachePayload(
			legacyPayload,
			destination,
			diagnostic),
			"a v1 timestamp-only entry must not be accepted as a v2 exact source revision");
		Require(diagnostic.find("sourceRevision") != std::string::npos,
			"the legacy payload diagnostic should name the missing source revision");
	}

	void TestCorruptPayloadDoesNotPartiallyPublish()
	{
		const FileId retainedId = MakeFileId("{ASSET-CACHE-RETAINED}");
		auto destination = MakeCache(
			retainedId.ToString(),
			"/workspace/Content/Retained.mat",
			6);

		const std::string corruptPayload =
			"assetCache:\n"
			"  assets:\n"
			"    '{ASSET-CACHE-CORRUPT}':\n"
			"      fileId: '{ASSET-CACHE-CORRUPT}'\n"
			"      assetImportTime: 7\n"
			"      sourcePath: ''\n"
			"      sourceRevision:\n"
			"        modificationTimeNanoseconds: 1\n"
			"        fileSize: 2\n"
			"        contentHash: 3\n";
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
			"      assetImportTime: 8\n"
			"      sourcePath: '/workspace/Content/Test.mat'\n"
			"      sourceRevision:\n"
			"        modificationTimeNanoseconds: 1\n"
			"        fileSize: 2\n"
			"        contentHash: 3\n";

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
			98);

		const std::string corruptPayload =
			"assetCache:\n"
			"  assets:\n"
			"    '{ASSET-CACHE-THROW}':\n"
			"      fileId: '{ASSET-CACHE-THROW}'\n"
			"      assetImportTime: abc\n"
			"      sourcePath: '/workspace/Content/Test.mat'\n"
			"      sourceRevision:\n"
			"        modificationTimeNanoseconds: 1\n"
			"        fileSize: 2\n"
			"        contentHash: 3\n";
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

	void TestImportAndUpdateCallbackContract()
	{
		TempDirectory directory("callback-contract");
		const std::filesystem::path sourcePath = directory.Path("Content/New.shader");
		WriteFile(sourcePath, "stages: []");

		TestAssetInfoHandler handler;
		RecordingAssetListener listener;
		handler.Subscribe(&listener);
		AssetInfoPtr imported = handler.ImportAsset(sourcePath.string(), {}, true, false);
		Require(imported != nullptr, "a new raw asset should be imported");
		Require(listener.m_events == std::vector<std::string>({ "update:false", "import" }),
			"a new raw asset must dispatch Update(false) followed by exactly one Import callback");
		Require(static_cast<TestAssetInfo*>(imported)->m_numMetaSaves == 1,
			"the import callback should finalize metadata exactly once");
		const YAML::Node importedMetadata = YAML::LoadFile(
			AssetRegistry::GetMetaFilePath(sourcePath.string()));
		Require(importedMetadata["assetInfoType"].as<std::string>() == "Sailor::AssetInfo",
			"the shared import path must add its handler's canonical AssetInfo type");
		delete imported;

		listener.m_events.clear();
		TestAssetInfo changed;
		changed.SetPendingUpdate(true);
		handler.NotifyUpdateAssetInfo(&changed);
		Require(listener.m_events == std::vector<std::string>({ "update:true" }),
			"an existing raw or metadata change must dispatch Update(true) without Import");
	}

	void TestImportNeverOverwritesExistingMetadata()
	{
		TempDirectory directory("import-preserves-metadata");
		const std::filesystem::path sourcePath = directory.Path("Content/New.shader");
		const std::filesystem::path metadataPath = directory.Path("Content/New.shader.asset");
		WriteFile(sourcePath, "stages: []");
		const std::string userMetadata =
			"fileId: '{USER-CREATED-METADATA}'\n"
			"filename: New.shader\n";
		WriteFile(metadataPath, userMetadata);

		TestAssetInfoHandler handler;
		RecordingAssetListener listener;
		handler.Subscribe(&listener);
		AssetInfoPtr imported = handler.ImportAsset(sourcePath.string(), {}, true, false);
		Require(imported == nullptr,
			"import must fail when a metadata sidecar already exists");
		std::ifstream metadataFile(metadataPath);
		const std::string preserved(
			(std::istreambuf_iterator<char>(metadataFile)),
			std::istreambuf_iterator<char>());
		metadataFile.close();
		Require(preserved == userMetadata,
			"import must never truncate or replace a user-created metadata sidecar");
		Require(listener.m_events.empty(),
			"a rejected import must not notify asset listeners");

		std::filesystem::remove(metadataPath);
		imported = handler.ImportAsset(sourcePath.string(), {}, false, false);
		Require(imported != nullptr,
			"a missing metadata sidecar should still be created exclusively");
		WriteFile(metadataPath, userMetadata);
		Require(!handler.DiscardImportedMetadataIfUnchanged(imported),
			"rollback must not delete metadata edited after exclusive creation");
		Require(std::filesystem::is_regular_file(metadataPath),
			"concurrently edited metadata must remain on disk");
		delete imported;

		std::filesystem::remove(metadataPath);
		imported = handler.ImportAsset(sourcePath.string(), {}, false, false);
		Require(imported != nullptr && handler.DiscardImportedMetadataIfUnchanged(imported),
			"rollback should remove an unchanged sidecar created by this import");
		Require(!std::filesystem::exists(metadataPath),
			"discarded generated metadata should be absent for the next import attempt");
		delete imported;
	}

	void TestRejectedReloadRestoresTheLiveAsset()
	{
		TempDirectory directory("reload-rollback");
		const std::filesystem::path sourcePath = directory.Path("Content/Existing.raw");
		const std::filesystem::path metadataPath = directory.Path("Content/Existing.raw.asset");
		WriteFile(sourcePath, "source");
		WriteFile(
			metadataPath,
			"fileId: '{ASSET-CACHE-RELOAD-ROLLBACK}'\n"
			"testValue: 99\n"
			"lateValue: invalid\n");

		TestAssetInfo info;
		const FileId fileId = MakeFileId("{ASSET-CACHE-RELOAD-ROLLBACK}");
		info.Configure(fileId, sourcePath, metadataPath);
		info.SetProcessingTimes(
			info.GetAssetLastModificationTime(),
			info.GetMetaLastModificationTime());
		info.m_testValue = 7;

		TestAssetInfoHandler handler;
		RecordingAssetListener listener;
		handler.Subscribe(&listener);
		Require(!handler.ReloadAssetInfo(&info, true, false),
			"a partially invalid metadata reload should be rejected");
		Require(info.GetFileId() == fileId && info.m_testValue == 7,
			"a rejected metadata reload must restore the previous live object state");
		Require(listener.m_events.empty(),
			"a rejected metadata reload must not notify importers");
	}

	void TestRawEditDispatchesExpiredUpdateWithoutImport()
	{
		TempDirectory directory("raw-expired-callback");
		const std::filesystem::path sourcePath = directory.Path("Content/Existing.raw");
		const std::filesystem::path metadataPath = directory.Path("Content/Existing.raw.asset");
		WriteFile(sourcePath, "source-v1");
		WriteFile(
			metadataPath,
			"fileId: '{ASSET-CACHE-RAW-EXPIRED}'\n"
			"testValue: 7\n"
			"lateValue: 1\n");

		const std::filesystem::file_time_type initialSourceTime =
			std::filesystem::file_time_type::clock::now() - std::chrono::seconds(20);
		std::filesystem::last_write_time(sourcePath, initialSourceTime);
		TestAssetInfo info;
		info.Configure(MakeFileId("{ASSET-CACHE-RAW-EXPIRED}"), sourcePath, metadataPath);
		info.SetProcessingTimes(
			info.GetAssetLastModificationTime(),
			info.GetMetaLastModificationTime());
		WriteFile(sourcePath, "source-v2");
		std::filesystem::last_write_time(sourcePath, initialSourceTime);
		Require(info.IsAssetExpired(),
			"a same-timestamp raw content change should expire by runtime revision");

		TestAssetInfoHandler handler;
		RecordingAssetListener listener;
		std::string reprocessedSource;
		listener.m_onExpiredUpdate = [&reprocessedSource](AssetInfoPtr updatedInfo)
		{
			std::ifstream source(updatedInfo->GetAssetFilepath(), std::ios::binary);
			reprocessedSource.assign(
				std::istreambuf_iterator<char>(source),
				std::istreambuf_iterator<char>());
		};
		handler.Subscribe(&listener);
		Require(handler.ReloadAssetInfo(&info, true, false),
			"an existing asset with a changed raw file should reload its metadata");
		Require(listener.m_events == std::vector<std::string>({ "update:true" }),
			"a raw edit must dispatch exactly Update(true) and never Import");
		Require(reprocessedSource == "source-v2",
			"Update(true) must let an importer reprocess the changed raw source");
		Require(!info.IsAssetExpired(),
			"a successfully dispatched raw edit should advance the in-memory processing watermark");
	}

	void TestMetadataEditDispatchesExpiredUpdateWithoutImport()
	{
		TempDirectory directory("meta-expired-callback");
		const std::filesystem::path sourcePath = directory.Path("Content/Existing.raw");
		const std::filesystem::path metadataPath = directory.Path("Content/Existing.raw.asset");
		WriteFile(sourcePath, "source");
		WriteFile(
			metadataPath,
			"fileId: '{ASSET-CACHE-META-EXPIRED}'\n"
			"testValue: 7\n"
			"lateValue: 1\n");

		const std::filesystem::file_time_type initialMetadataTime =
			std::filesystem::file_time_type::clock::now() - std::chrono::seconds(20);
		std::filesystem::last_write_time(metadataPath, initialMetadataTime);
		TestAssetInfo info;
		info.Configure(MakeFileId("{ASSET-CACHE-META-EXPIRED}"), sourcePath, metadataPath);
		info.SetProcessingTimes(
			info.GetAssetLastModificationTime(),
			info.GetMetaLastModificationTime());

		WriteFile(
			metadataPath,
			"fileId: '{ASSET-CACHE-META-EXPIRED}'\n"
			"testValue: 9\n"
			"lateValue: 1\n");
		std::filesystem::last_write_time(metadataPath, initialMetadataTime);
		Require(info.IsMetaExpired(),
			"same-timestamp metadata content changes should expire by runtime revision");

		TestAssetInfoHandler handler;
		RecordingAssetListener listener;
		handler.Subscribe(&listener);
		Require(handler.ReloadAssetInfo(&info, true, false),
			"an existing asset with changed metadata should reload");
		Require(listener.m_events == std::vector<std::string>({ "update:true" }),
			"a metadata edit must dispatch exactly Update(true) and never Import");
		Require(info.m_testValue == 9,
			"Update(true) should observe the newly loaded metadata values");
		Require(!info.IsMetaExpired(),
			"a successfully dispatched metadata edit should advance its processing watermark");
	}

	void TestConcurrentMetadataEditDoesNotAdvanceTheWatermark()
	{
		TempDirectory directory("reload-concurrent-edit");
		const std::filesystem::path sourcePath = directory.Path("Content/Existing.raw");
		const std::filesystem::path metadataPath = directory.Path("Content/Existing.raw.asset");
		WriteFile(sourcePath, "source");
		WriteFile(
			metadataPath,
			"fileId: '{ASSET-CACHE-RELOAD-CONCURRENT}'\n"
			"testValue: 99\n"
			"lateValue: 1\n");

		TestAssetInfo info;
		const FileId fileId = MakeFileId("{ASSET-CACHE-RELOAD-CONCURRENT}");
		info.Configure(fileId, sourcePath, metadataPath);
		info.SetProcessingTimes(
			info.GetAssetLastModificationTime(),
			info.GetMetaLastModificationTime());
		info.m_testValue = 7;
		const std::filesystem::file_time_type initialMetaTime =
			std::filesystem::last_write_time(metadataPath);
		info.m_onDeserialize = [&]()
		{
			std::filesystem::last_write_time(
				metadataPath,
				initialMetaTime + std::chrono::seconds(2));
		};

		TestAssetInfoHandler handler;
		RecordingAssetListener listener;
		handler.Subscribe(&listener);
		Require(!handler.ReloadAssetInfo(&info, true, false),
			"metadata modified during deserialization should reject the reload");
		Require(info.GetFileId() == fileId && info.m_testValue == 7,
			"a concurrent metadata edit must restore the previous live state");
		Require(info.IsMetaExpired(),
			"the concurrently edited metadata must remain expired for the next reload");
		Require(listener.m_events.empty(),
			"a concurrent metadata edit must not notify importers or advance processing watermarks");
	}

	void TestUpdateTracksAssetImportStateAndPreservesDirtyState()
	{
		TestAssetCache cache;
		const FileId fileId = MakeFileId("{ASSET-CACHE-UPDATE}");
		const std::string sourcePath = "/workspace/Content/Test.mat";
		const FileRevision sourceRevision = MakeRevision();

		Require(cache.Update(fileId, 90, sourcePath, sourceRevision),
			"new asset import state should change the cache");
		Require(cache.IsDirty(), "new asset import state should mark the cache dirty");
		Require(!cache.Update(fileId, 90, sourcePath, sourceRevision),
			"unchanged asset import state should be a no-op");
		Require(cache.IsDirty(), "a no-op update must not clear an already dirty cache");
		Require(cache.Update(fileId, 91, sourcePath, sourceRevision),
			"a later successful asset import should change the source watermark");
		Require(cache.Update(fileId, 91, sourcePath, MakeRevision(123456789, 64, 42)),
			"a changed content hash should change the exact source revision");
		Require(cache.Update(fileId, 91, sourcePath + ".moved", MakeRevision(123456789, 64, 42)),
			"a source path change should change the cache");
	}

	void TestPruneRemovesOnlyEntriesOutsideTheCommittedGeneration()
	{
		TestAssetCache cache;
		const FileId liveFileId = MakeFileId("{ASSET-CACHE-PRUNE-LIVE}");
		const FileId staleFileId = MakeFileId("{ASSET-CACHE-PRUNE-STALE}");
		Require(cache.Update(liveFileId, 10, "/workspace/Content/Live.mat", MakeRevision()),
			"the live entry should populate the cache");
		Require(cache.Update(staleFileId, 20, "/workspace/Content/Stale.mat", MakeRevision()),
			"the stale entry should populate the cache");

		TSet<FileId> liveAssetIds;
		liveAssetIds.Insert(liveFileId);
		Require(cache.Prune(liveAssetIds),
			"pruning a committed generation should report removed stale entries");
		Require(cache.Contains(liveFileId),
			"pruning must preserve entries from the committed registry generation");
		Require(!cache.Contains(staleFileId),
			"pruning must remove entries absent from the committed registry generation");
		Require(!cache.Prune(liveAssetIds),
			"pruning an already synchronized cache should be a no-op");
	}

	void TestRestoreChangesOnlyAssetImportTime()
	{
		TestAssetCache cache;
		const FileId fileId = MakeFileId("{ASSET-CACHE-RUNTIME-METADATA}");
		TempDirectory directory("restore-import-time");
		const std::filesystem::path sourcePath = directory.Path("Content/Test.mat");
		const std::filesystem::path metadataPath = directory.Path("Content/Test.mat.asset");
		WriteFile(sourcePath, "source");
		WriteFile(metadataPath, "metadata");
		FileRevision sourceRevision;
		Require(Utils::TryGetFileRevision(sourcePath.string(), sourceRevision),
			"the source revision fixture should be readable");
		Require(cache.Update(fileId, 123, sourcePath.string(), sourceRevision),
			"asset import state should populate the cache");

		TestAssetInfo info;
		info.Configure(fileId, sourcePath, metadataPath);
		info.SetProcessingTimes(0, 456);
		Require(cache.RestoreAssetImportTime(&info, sourceRevision),
			"matching file id and source path should restore the asset import time");
		Require(info.GetAssetImportTime() == 123,
			"the persisted asset import time should be restored");
		Require(info.GetRuntimeMetadataLoadTime() == 456,
			"restoring the asset cache must not change runtime metadata load time");
	}

	void TestAssetImportCacheAndRuntimeMetadataExpiration()
	{
		TempDirectory directory("import-and-runtime-metadata");
		const std::filesystem::path sourcePath = directory.Path("Content/Test.mat");
		const std::filesystem::path metadataPath = directory.Path("Content/Test.mat.asset");
		WriteFile(sourcePath, "source");
		WriteFile(metadataPath, "metadata");

		const auto initialTime = std::filesystem::file_time_type::clock::now() - std::chrono::seconds(20);
		std::filesystem::last_write_time(sourcePath, initialTime);
		std::filesystem::last_write_time(metadataPath, initialTime);

		TestAssetInfo info;
		info.Configure(MakeFileId("{ASSET-CACHE-FILESYSTEM}"), sourcePath, metadataPath);
		info.SetProcessingTimes(
			info.GetAssetLastModificationTime(),
			info.GetMetaLastModificationTime());
		TestAssetCache cache;
		Require(cache.Update(&info), "the initial asset import state should populate the cache");
		Require(!cache.IsExpired(&info), "an unchanged source file should remain current in the cache");

		WriteFile(sourcePath, "source-v2");
		std::filesystem::last_write_time(sourcePath, initialTime + std::chrono::seconds(2));
		Require(cache.IsExpired(&info), "a newer source file should expire the cache");
		Require(!cache.Update(&info),
			"cache update must not acknowledge a source revision that AssetInfo did not process");
		Require(cache.IsExpired(&info),
			"a rejected acknowledgement must preserve the previous cache revision");
		info.SetProcessingTimes(
			info.GetAssetLastModificationTime(),
			info.GetMetaLastModificationTime());
		Require(cache.Update(&info), "the asset import watermark should advance");
		Require(!cache.IsExpired(&info), "the processed source version should become current");

		const auto backdatedTime = initialTime - std::chrono::seconds(2);
		WriteFile(sourcePath, "source-v3");
		std::filesystem::last_write_time(sourcePath, backdatedTime);
		Require(cache.IsExpired(&info),
			"a backdated same-size content change must expire the cache");
		info.SetProcessingTimes(
			info.GetAssetLastModificationTime(),
			info.GetMetaLastModificationTime());
		Require(cache.Update(&info),
			"a successfully processed backdated revision should update the cache");
		Require(!cache.IsExpired(&info),
			"the acknowledged backdated revision should become current");

		WriteFile(sourcePath, "source-v4");
		std::filesystem::last_write_time(sourcePath, backdatedTime);
		Require(cache.IsExpired(&info),
			"a same-timestamp same-size content change must expire by content hash");
		info.SetProcessingTimes(
			info.GetAssetLastModificationTime(),
			info.GetMetaLastModificationTime());
		Require(cache.Update(&info),
			"the same-timestamp content revision should be acknowledgeable");
		Require(!cache.IsExpired(&info),
			"the processed same-timestamp revision should become current");
		const std::time_t importedSourceTime = info.GetAssetImportTime();

		std::filesystem::last_write_time(metadataPath, initialTime + std::chrono::seconds(4));
		Require(!cache.IsExpired(&info),
			"metadata changes must not participate in persisted asset cache expiration");
		Require(info.IsMetaExpired(),
			"a newer metadata file must remain detectable through runtime AssetInfo state");
		Require(!cache.Update(&info),
			"metadata load time changes must not dirty the persisted asset cache");
		info.SetProcessingTimes(
			importedSourceTime,
			info.GetMetaLastModificationTime());
		Require(!info.IsMetaExpired(),
			"advancing runtime metadata load time should acknowledge the loaded metadata");

		std::filesystem::remove(metadataPath);
		Require(info.IsMetaExpired(), "a missing metadata file should expire runtime AssetInfo state");
		Require(!cache.IsExpired(&info),
			"missing metadata must not be represented as a persisted cache timestamp or path");
		Require(!cache.Update(&info),
			"missing metadata must not dirty or remove persisted source import state");
		Require(!cache.IsExpired(&info),
			"runtime metadata validity must remain independent from persisted source import state");
	}

	void TestRuntimeMetadataFieldsAreRejectedTransactionally()
	{
		const FileId retainedId = MakeFileId("{ASSET-CACHE-STRICT-RETAINED}");
		const std::string prefix =
			"assetCache:\n"
			"  assets:\n"
			"    '{ASSET-CACHE-STRICT}':\n"
			"      fileId: '{ASSET-CACHE-STRICT}'\n"
			"      assetImportTime: 1\n"
			"      sourcePath: '/workspace/Content/Test.mat'\n"
			"      sourceRevision:\n"
			"        modificationTimeNanoseconds: 1\n"
			"        fileSize: 2\n"
			"        contentHash: 3\n";
		const std::string invalidPayloads[] =
		{
			prefix + "      metadataLoadTime: 2\n",
			prefix + "      metadataPath: '/workspace/Content/Test.mat.asset'\n",
			prefix +
				"      metadataLoadTime: 2\n"
				"      metadataPath: '/workspace/Content/Test.mat.asset'\n",
			prefix +
				"      unexpectedField: 3\n"
		};

		for (const std::string& payload : invalidPayloads)
		{
			auto destination = MakeCache(
				retainedId.ToString(),
				"/workspace/Content/Retained.mat",
				6);
			std::string diagnostic;
			Require(!TestAssetCache::TryDeserializeAssetCachePayload(payload, destination, diagnostic),
				"runtime metadata fields and other unknown fields must be rejected");
			Require(!diagnostic.empty(), "strict persisted schema validation should return a diagnostic");
			Require(destination.m_data.Num() == 1 && destination.m_data.ContainsKey(retainedId),
				"strict schema validation failures must not partially publish state");
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

	void TestAssetProcessingTokenRejectsStaleCompletion()
	{
		AssetRegistry::AssetProcessingToken current;
		current.m_fileId = MakeFileId("{ASSET-PROCESSING-TOKEN}");
		current.m_sourceRevision = MakeRevision(123, 64, 456);
		current.m_sourcePath = "/workspace/Content/Retry.shader";
		current.m_assetImportTime = 10;
		current.m_generation = 2;
		Require(current.Matches(current),
			"an exact processing completion should match its current generation");

		AssetRegistry::AssetProcessingToken staleGeneration = current;
		staleGeneration.m_generation = 1;
		Require(!current.Matches(staleGeneration),
			"an older async completion must not acknowledge a newer generation");

		AssetRegistry::AssetProcessingToken staleRevision = current;
		staleRevision.m_sourceRevision = MakeRevision(124, 64, 789);
		Require(!current.Matches(staleRevision),
			"a completion for another source revision must remain pending for retry");

		AssetRegistry::AssetProcessingToken stalePath = current;
		stalePath.m_sourcePath = "/workspace/Content/Moved.shader";
		Require(!current.Matches(stalePath),
			"a completion for another source path must not acknowledge the current asset");

		AssetRegistry::AssetProcessingToken staleImportTime = current;
		staleImportTime.m_assetImportTime = 9;
		Require(!current.Matches(staleImportTime),
			"a completion for another source timestamp must not acknowledge the current asset");
	}
}

int main()
{
	try
	{
		TestPayloadRoundTrip();
		TestEmptyPayloadRoundTrip();
		TestLegacyTimestampOnlyPayloadIsRejected();
		TestCorruptPayloadDoesNotPartiallyPublish();
		TestMismatchedEntryIdentityIsCorrupt();
		TestDirectDeserializeDoesNotThrowOnCorruptData();
		TestEntryDeserializeDoesNotThrow();
		TestEveryAssetInfoSerializerWritesItsConcreteType();
		TestGeneratedGlbMetadataUsesTypedDefaults();
		TestImportAndUpdateCallbackContract();
		TestImportNeverOverwritesExistingMetadata();
		TestRejectedReloadRestoresTheLiveAsset();
		TestRawEditDispatchesExpiredUpdateWithoutImport();
		TestMetadataEditDispatchesExpiredUpdateWithoutImport();
		TestConcurrentMetadataEditDoesNotAdvanceTheWatermark();
		TestUpdateTracksAssetImportStateAndPreservesDirtyState();
		TestPruneRemovesOnlyEntriesOutsideTheCommittedGeneration();
		TestRestoreChangesOnlyAssetImportTime();
		TestAssetImportCacheAndRuntimeMetadataExpiration();
		TestRuntimeMetadataFieldsAreRejectedTransactionally();
		TestIoFailurePreservesTheExistingCacheFile();
		TestAssetProcessingTokenRejectsStaleCompletion();
		std::cout << "Asset cache contract tests passed.\n";
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::cerr << "Asset cache contract tests failed: " << exception.what() << '\n';
		return 1;
	}
}
