#include "AssetRegistry/AssetCache.h"
#include "AssetRegistry/AssetScanSourceRevisionCache.h"
#include "AssetRegistry/Animation/AnimationAssetInfo.h"
#include "AssetRegistry/Animation/AnimationControllerAssetInfo.h"
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
#include "Tasks/Scheduler.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
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
		using AssetCache::Remove;
		using AssetCache::Contains;
		using AssetCache::ShouldResetCacheFile;
		using AssetCache::ShouldWriteCacheFile;
		using AssetCache::TryDeserializeAssetCachePayload;
		using AssetCache::Update;
		using AssetCache::RestoreAssetImportTime;

		bool Update(
			const FileId& id,
			std::time_t assetImportTime,
			const std::string& sourcePath,
			const FileRevision& sourceRevision)
		{
			return AssetCache::Update(
				id,
				assetImportTime,
				sourcePath,
				sourceRevision,
				std::filesystem::path(sourcePath).filename().string() + ".asset",
				sourceRevision,
				"Sailor::AssetInfo");
		}
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

	class LazyAssetInfoLoadingScope final
	{
	public:
		explicit LazyAssetInfoLoadingScope(bool bEnabled) :
			m_bPrevious(g_bUseLazyAssetInfoLoading)
		{
			g_bUseLazyAssetInfoLoading = bEnabled;
		}

		~LazyAssetInfoLoadingScope()
		{
			g_bUseLazyAssetInfoLoading = m_bPrevious;
		}

	private:
		bool m_bPrevious = false;
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
			info->SetProcessingTimes(
				info->GetAssetLastModificationTime(),
				info->GetMetaLastModificationTime());
			info->SetPendingUpdate(true);
			std::function<void()> onLoad;
			onLoad.swap(m_onLoad);
			if (onLoad)
			{
				onLoad();
			}
			return info;
		}

		mutable std::function<void()> m_onLoad;

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

	class TargetedUpdateAssetInfo final : public AssetInfo
	{
	public:
		explicit TargetedUpdateAssetInfo(IAssetInfoHandler* handler) :
			m_handler(handler)
		{
		}

		YAML::Node Serialize() const override
		{
			YAML::Node result(YAML::NodeType::Map);
			result["fileId"] = m_fileId;
			result["filename"] = m_assetFilename;
			result["testValue"] = m_testValue;
			return result;
		}

		void Deserialize(const YAML::Node& inData) override
		{
			m_fileId = inData["fileId"].as<FileId>();
			m_assetFilename = inData["filename"].as<std::string>();
			m_testValue = inData["testValue"].as<int32_t>();
		}

		IAssetInfoHandler* GetHandler() override
		{
			return m_handler;
		}

		int32_t GetTestValue() const noexcept
		{
			return m_testValue;
		}

	private:
		IAssetInfoHandler* m_handler = nullptr;
		int32_t m_testValue = 0;
	};

	class TargetedUpdateAssetInfoHandler final : public IAssetInfoHandler
	{
	public:
		void GetDefaultMeta(YAML::Node& outDefaultYaml) const override
		{
			outDefaultYaml = YAML::Node(YAML::NodeType::Map);
		}

	protected:
		AssetInfoPtr CreateAssetInfo() const override
		{
			return new TargetedUpdateAssetInfo(
				const_cast<TargetedUpdateAssetInfoHandler*>(this));
		}
	};

	class RecordingTargetedUpdateListener final : public IAssetInfoHandlerListener
	{
	public:
		void OnUpdateAssetInfo(
			AssetInfoPtr assetInfo,
			bool bWasExpired) override
		{
			m_updatedFileIds.emplace_back(assetInfo->GetFileId());
			m_expirationFlags.emplace_back(bWasExpired);
			if (m_onUpdate)
			{
				m_onUpdate(assetInfo, bWasExpired);
			}
		}

		void OnImportAsset(AssetInfoPtr) override
		{
		}

		void Clear()
		{
			m_updatedFileIds.clear();
			m_expirationFlags.clear();
		}

		std::vector<FileId> m_updatedFileIds;
		std::vector<bool> m_expirationFlags;
		std::function<void(AssetInfoPtr, bool)> m_onUpdate;
	};

	class TestMainThreadTask final : public Tasks::ITask
	{
	public:
		explicit TestMainThreadTask(std::function<void()> callback) :
			ITask("Asset cache pre-commit metadata mutation", EThreadType::Main),
			m_callback(std::move(callback))
		{
		}

		void Execute() override
		{
			m_state |= StateMask::IsStartedBit;
			if (m_callback)
			{
				m_callback();
			}
			m_state |= StateMask::IsFinishedBit;
		}

	private:
		std::function<void()> m_callback;
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

	void TestNewFileIdsUseCrossPlatformGuidFormatting()
	{
		const FileId braced = MakeFileId("{7810180E-F1BB-4C88-9C9B-28ED9B086974}");
		const FileId plain = MakeFileId("7810180E-F1BB-4C88-9C9B-28ED9B086974");
		const FileId lowercase = MakeFileId("7810180e-f1bb-4c88-9c9b-28ed9b086974");
		Require(braced == plain && plain == lowercase,
			"Windows and macOS GUID spellings must resolve to the same FileId");
		Require(braced.ToString() == "7810180E-F1BB-4C88-9C9B-28ED9B086974",
			"deserialized GUID FileIds must use uppercase unbraced canonical form");
		Require(braced.Serialize().as<std::string>() == plain.ToString(),
			"serialized GUID FileIds must remain portable across platforms");
		Require(MakeFileId("{ASSET-CACHE-SYMBOLIC}").ToString() ==
			"{ASSET-CACHE-SYMBOLIC}",
			"non-GUID symbolic FileIds must remain unchanged");
		Require(MakeFileId("7810180E-F1BB-4C88-9C9B-INVALID-GUID").ToString() ==
			"7810180E-F1BB-4C88-9C9B-INVALID-GUID",
			"malformed GUID-like FileIds must remain unchanged");

		const std::string generated = FileId::CreateNewFileId().ToString();
		Require(generated.size() == 36 && generated.front() != '{' && generated.back() != '}',
			"new FileIds must use the same unbraced GUID representation on every platform");
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
		RequireSerializedAssetInfoType<AnimationControllerAssetInfo>("Sailor::AnimationControllerAssetInfo");
		RequireSerializedAssetInfoType<AnimationSetAssetInfo>("Sailor::AnimationSetAssetInfo");
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
		entry.m_metadataFilename =
			std::filesystem::path(sourcePath).filename().string() + ".asset";
		entry.m_metadataRevision = MakeRevision();
		entry.m_assetInfoType = "Sailor::AssetInfo";
		result.m_assets.Insert(fileId, std::move(entry));
		return result;
	}

	void WriteFile(const std::filesystem::path& path, const std::string& content)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream stream(path, std::ios::binary);
		stream << content;
	}

	void RewriteFileWithNewRevision(
		const std::filesystem::path& path,
		const std::string& content)
	{
		const std::filesystem::file_time_type previousWriteTime =
			std::filesystem::last_write_time(path);
		WriteFile(path, content);
		std::error_code timestampError;
		std::filesystem::last_write_time(
			path,
			previousWriteTime + std::chrono::seconds(2),
			timestampError);
		Require(
			!timestampError,
			"the targeted update fixture must advance the file revision");
	}

	Workspace::WorkspaceContext CreateWorkspaceContext(
		const TempDirectory& directory)
	{
		const std::filesystem::path workspaceRoot =
			directory.Path("Workspace");
		std::filesystem::create_directories(
			directory.Path("Engine/Content"));
		std::filesystem::create_directories(
			workspaceRoot / "Content");
		WriteFile(
			workspaceRoot / "workspace.sailor",
			"manifestVersion: 1\n"
			"workspaceId: 00000000-0000-0000-0000-000000000131\n"
			"name: Asset Cache Contract\n"
			"enginePath: ../Engine\n"
			"engineReferenceKind: source\n"
			"contentPath: Content\n"
			"sourcePath: Source\n"
			"generatedProjectPath: Generated\n"
			"cachePath: Cache\n"
			"buildPath: Cache/Build\n"
			"logicOutputPath: Binaries\n"
			"logicModuleName: AssetCacheContract\n");
		const Workspace::WorkspaceContextResolveResult result =
			Workspace::ResolveWorkspaceContext(
				workspaceRoot,
				workspaceRoot / "workspace.sailor");
		Require(
			result.IsSuccess(),
			"asset cache contract workspace should resolve: " +
				result.m_message);
		return result.m_context;
	}

	void WriteScanAssetFixture(
		const Workspace::WorkspaceContext& workspaceContext,
		const std::string& source = "source-v1")
	{
		WriteFile(
			workspaceContext.GetContent() / "Retry.raw",
			source);
		WriteFile(
			workspaceContext.GetContent() / "Retry.raw.asset",
			"fileId: '{ASSET-CACHE-IMPORT-CONTRACT}'\n"
			"filename: Retry.raw\n"
			"testValue: 7\n"
			"lateValue: 1\n");
	}

	void WriteTargetedUpdateFixture(
		const Workspace::WorkspaceContext& workspaceContext)
	{
		WriteFile(
			workspaceContext.GetContent() / "Shared.raw",
			"shared-source-v1");
		WriteFile(
			workspaceContext.GetContent() / "Shared.raw.asset",
			"fileId: '{TARGETED-UPDATE-PRIMARY}'\n"
			"filename: Shared.raw\n"
			"testValue: 1\n");
		WriteFile(
			workspaceContext.GetContent() / "Shared.raw2.asset",
			"fileId: '{TARGETED-UPDATE-SECONDARY}'\n"
			"filename: Shared.raw\n"
			"testValue: 2\n");
	}

	void RegisterRawHandler(
		AssetRegistry& registry,
		TestAssetInfoHandler& handler)
	{
		TVector<std::string> extensions;
		extensions.Add("raw");
		Require(
			registry.RegisterAssetInfoHandler(extensions, &handler),
			"the scan fixture handler should register for raw assets");
	}

	void RegisterTargetedUpdateHandler(
		AssetRegistry& registry,
		TargetedUpdateAssetInfoHandler& handler)
	{
		TVector<std::string> extensions;
		extensions.Add("raw");
		extensions.Add("raw2");
		Require(
			registry.RegisterAssetInfoHandler(extensions, &handler),
			"the targeted update handler should register for raw assets");
	}

	void TestLazyScanDefersUnchangedMetadataMaterialization()
	{
		LazyAssetInfoLoadingScope lazyLoading(true);
		TempDirectory directory("lazy-materialization");
		const Workspace::WorkspaceContext workspaceContext =
			CreateWorkspaceContext(directory);
		WriteScanAssetFixture(workspaceContext);
		{
			TestAssetInfoHandler handler;
			AssetRegistry registry(workspaceContext);
			RegisterRawHandler(registry, handler);
			Require(registry.ScanContentFolder() &&
				registry.CompleteScanProcessing(),
				"the empty v1 cache should be built by the existing eager scan");
		}

		const std::filesystem::path cachePath =
			workspaceContext.GetCache() / "AssetCache.yaml";
		std::ifstream cacheFile(cachePath, std::ios::binary);
		const std::string cacheContents{
			std::istreambuf_iterator<char>(cacheFile),
			std::istreambuf_iterator<char>()};
		Require(cacheContents.find("producerIdentity: asset-cache-v1") != std::string::npos &&
			cacheContents.find("payloadVersion: 1") != std::string::npos &&
			cacheContents.find("asset-cache-v2") == std::string::npos,
			"the rebuilt cache must use only the strict asset-cache-v1 identity");

		uint32_t numMetadataLoads = 0;
		TestAssetInfoHandler handler;
		AssetRegistry registry(workspaceContext);
		handler.m_onLoad = [&]() { ++numMetadataLoads; };
		RegisterRawHandler(registry, handler);
		Require(registry.ScanContentFolder() &&
			registry.CompleteScanProcessing(),
			"the current v1 cache should initialize the lazy registry index");
		Require(numMetadataLoads == 0,
			"an unchanged lazy scan must not read asset metadata");

		AssetInfoPtr materialized = registry.GetAssetInfoPtr(
			(workspaceContext.GetContent() / "Retry.raw").string());
		Require(materialized != nullptr && numMetadataLoads == 1,
			"the first concrete lookup should materialize exactly one AssetInfo proxy");
		Require(registry.GetAssetInfoPtr(materialized->GetFileId()) == materialized &&
			numMetadataLoads == 1,
			"subsequent lookups should reuse the materialized AssetInfo");
	}

	void TestV2EnvelopeIsResetInsteadOfMigrated()
	{
		TempDirectory directory("reject-v2");
		const Workspace::WorkspaceContext workspaceContext =
			CreateWorkspaceContext(directory);
		const FileId oldFileId = MakeFileId("{ASSET-CACHE-OLD-V2}");
		const auto oldPayload = TestAssetCache::SerializeAssetCachePayload(
			MakeCache(
				oldFileId.ToString(),
				(workspaceContext.GetContent() / "Old.mat").string(),
				10));
		const Workspace::WorkspaceCacheIdentity oldIdentity =
			Workspace::MakeWorkspaceCacheIdentity(
				"asset-cache",
				"asset-cache-v2",
				2,
				workspaceContext);
		std::string oldEnvelope;
		std::string diagnostic;
		Require(Workspace::SerializeWorkspaceCacheEnvelope(
			oldIdentity,
			oldPayload,
			oldEnvelope,
			diagnostic),
			"the old-version rejection fixture should serialize: " + diagnostic);
		WriteFile(
			workspaceContext.GetCache() / "AssetCache.yaml",
			oldEnvelope);

		AssetCache cache;
		cache.Initialize(workspaceContext);
		Require(
			cache.GetLastLoadResult().m_status ==
				Workspace::EWorkspaceCacheLoadStatus::UnsupportedVersion,
			"an asset-cache-v2 envelope must be rejected instead of migrated");
		Require(!cache.Contains(oldFileId),
			"resetting the unsupported envelope must not publish old cache entries");

		std::ifstream cacheFile(
			workspaceContext.GetCache() / "AssetCache.yaml",
			std::ios::binary);
		const std::string rebuiltEnvelope{
			std::istreambuf_iterator<char>(cacheFile),
			std::istreambuf_iterator<char>()};
		Require(rebuiltEnvelope.find("producerIdentity: asset-cache-v1") != std::string::npos &&
			rebuiltEnvelope.find("payloadVersion: 1") != std::string::npos &&
			rebuiltEnvelope.find("asset-cache-v2") == std::string::npos,
			"the unsupported cache must be replaced directly with an empty strict v1 envelope");
	}

	void TestLazyScanLoadsOnlyNewSecondaryMetadata()
	{
		LazyAssetInfoLoadingScope lazyLoading(true);
		TempDirectory directory("lazy-new-secondary");
		const Workspace::WorkspaceContext workspaceContext =
			CreateWorkspaceContext(directory);
		WriteFile(
			workspaceContext.GetContent() / "Shared.raw",
			"source");
		WriteFile(
			workspaceContext.GetContent() / "Shared.raw.asset",
			"fileId: '{LAZY-PRIMARY}'\n"
			"filename: Shared.raw\n"
			"testValue: 1\n");
		{
			TargetedUpdateAssetInfoHandler handler;
			AssetRegistry registry(workspaceContext);
			RegisterTargetedUpdateHandler(registry, handler);
			Require(registry.ScanContentFolder() &&
				registry.CompleteScanProcessing(),
				"the initial primary asset should build the strict v1 index");
		}

		WriteFile(
			workspaceContext.GetContent() / "Shared.raw2.asset",
			"fileId: '{LAZY-SECONDARY}'\n"
			"filename: Shared.raw\n"
			"testValue: 2\n");
		RecordingTargetedUpdateListener listener;
		TargetedUpdateAssetInfoHandler handler;
		handler.Subscribe(&listener);
		AssetRegistry registry(workspaceContext);
		RegisterTargetedUpdateHandler(registry, handler);
		Require(registry.ScanContentFolder() &&
			registry.CompleteScanProcessing(),
			"the lazy scan should register new secondary metadata");
		Require(listener.m_updatedFileIds.size() == 1 &&
			listener.m_updatedFileIds[0] == MakeFileId("{LAZY-SECONDARY}"),
			"the lazy scan should read only the newly discovered secondary AssetInfo");
		Require(registry.GetAssetInfoPtr(
				MakeFileId("{LAZY-SECONDARY}")) != nullptr,
			"the new secondary AssetInfo should be immediately resolvable");
	}

	void TestScanCanonicalizesUuidMetadataIdentity()
	{
		TempDirectory directory("canonical-uuid-scan");
		const Workspace::WorkspaceContext workspaceContext =
			CreateWorkspaceContext(directory);
		const FileId portableId =
			MakeFileId("7810180E-F1BB-4C88-9C9B-28ED9B086974");
		WriteFile(
			workspaceContext.GetContent() / "Portable.raw",
			"portable-source");
		WriteFile(
			workspaceContext.GetContent() / "Portable.raw.asset",
			"fileId: '{7810180E-F1BB-4C88-9C9B-28ED9B086974}'\n"
			"filename: Portable.raw\n"
			"testValue: 9\n");

		{
			TargetedUpdateAssetInfoHandler handler;
			AssetRegistry registry(workspaceContext);
			RegisterTargetedUpdateHandler(registry, handler);
			Require(registry.ScanContentFolder() &&
				registry.CompleteScanProcessing(),
				"a Windows-style braced UUID must survive staged metadata loading");

			AssetInfoPtr assetInfo = registry.GetAssetInfoPtr(portableId);
			Require(assetInfo != nullptr &&
				assetInfo->GetFileId().ToString() == portableId.ToString(),
				"the staged registry must publish the canonical cross-platform UUID");
		}

		AssetCache reloadedCache;
		reloadedCache.Initialize(workspaceContext);
		Require(reloadedCache.Contains(portableId),
			"the canonical UUID must remain resolvable after reloading the asset cache");
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
		Require(serializedEntry.IsMap() && serializedEntry.size() == 7,
			"persisted asset cache v1 entries must contain the watermark and lazy index fields");
		const YAML::Node serializedRevision = serializedEntry["sourceRevision"];
		Require(serializedRevision.IsMap() && serializedRevision.size() == 3,
			"persisted source revisions must contain mtime, size, and content hash");
		Require(payload.find("metadataLoadTime") == std::string::npos &&
			payload.find("metadataPath") == std::string::npos &&
			serializedEntry["metadataFilename"].as<std::string>() == "Test.mat.asset",
			"the cache must persist only the colocated metadata filename, never runtime metadata state or a metadata path");

		TestAssetCache::CacheData loaded;
		std::string diagnostic;
		Require(
			TestAssetCache::TryDeserializeAssetCachePayload(payload, loaded, diagnostic),
			"current asset payload should load: " + diagnostic);
		Require(loaded.m_assets.ContainsKey(fileId), "round-tripped payload should retain its file id");
		const auto entry = loaded.m_assets[fileId];
		Require(entry.m_fileId == fileId, "round-tripped entry identity should match its map key");
		Require(entry.m_assetImportTime == 40, "round-tripped asset import time should be preserved");
		Require(!entry.m_sourcePath.empty(), "round-tripped source path should be normalized");
		const FileRevision expectedRevision = MakeRevision();
		Require(
			entry.m_sourceRevision.m_modificationTimeNanoseconds ==
				expectedRevision.m_modificationTimeNanoseconds &&
			entry.m_sourceRevision.m_fileSize == expectedRevision.m_fileSize &&
			entry.m_sourceRevision.m_contentHash == expectedRevision.m_contentHash &&
			entry.m_sourceRevision.m_bIsValid == expectedRevision.m_bIsValid,
			"round-tripped serialized source revision fields should be preserved");
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
		Require(loaded.m_assets.Num() == 0, "empty asset payload should remain empty");
	}

	void TestPreV1PayloadIsRejected()
	{
		const std::string preV1Payload =
			"assetCache:\n"
			"  assets:\n"
			"    '{ASSET-CACHE-PRE-V1}':\n"
			"      fileId: '{ASSET-CACHE-PRE-V1}'\n"
			"      assetImportTime: 7\n"
			"      sourcePath: '/workspace/Content/Test.mat'\n";
		TestAssetCache::CacheData destination;
		std::string diagnostic;
		Require(!TestAssetCache::TryDeserializeAssetCachePayload(
			preV1Payload,
			destination,
			diagnostic),
			"a pre-v1 entry must not be accepted by the strict v1 cache schema");
		Require(!diagnostic.empty(),
			"the rejected pre-v1 payload should return an actionable diagnostic");
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
			"        contentHash: 3\n"
			"      metadataFilename: Test.mat.asset\n"
			"      metadataRevision:\n"
			"        modificationTimeNanoseconds: 1\n"
			"        fileSize: 2\n"
			"        contentHash: 3\n"
			"      assetInfoType: Sailor::MaterialAssetInfo\n";
		std::string diagnostic;
		Require(
			!TestAssetCache::TryDeserializeAssetCachePayload(corruptPayload, destination, diagnostic),
			"empty sourcePath should reject the complete payload");
		Require(!diagnostic.empty(), "corrupt asset payload should return an actionable diagnostic");
		Require(destination.m_assets.Num() == 1 && destination.m_assets.ContainsKey(retainedId),
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
			"        contentHash: 3\n"
			"      metadataFilename: Test.mat.asset\n"
			"      metadataRevision:\n"
			"        modificationTimeNanoseconds: 1\n"
			"        fileSize: 2\n"
			"        contentHash: 3\n"
			"      assetInfoType: Sailor::MaterialAssetInfo\n";

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
			"        contentHash: 3\n"
			"      metadataFilename: Test.mat.asset\n"
			"      metadataRevision:\n"
			"        modificationTimeNanoseconds: 1\n"
			"        fileSize: 2\n"
			"        contentHash: 3\n"
			"      assetInfoType: Sailor::MaterialAssetInfo\n";
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

		Require(destination.m_assets.Num() == 0, "corrupt direct deserialize payload should not leave stale data");
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
		std::filesystem::last_write_time(
			sourcePath,
			initialSourceTime + std::chrono::seconds(2));
		Require(info.IsAssetExpired(),
			"a newer raw source timestamp should expire the runtime revision");

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
		std::filesystem::last_write_time(
			metadataPath,
			initialMetadataTime + std::chrono::seconds(2));
		Require(info.IsMetaExpired(),
			"a newer metadata timestamp should expire the runtime revision");

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
		Require(!cache.Update(fileId, 91, sourcePath, MakeRevision(123456789, 64, 42)),
			"content hash changes must not affect timestamp-based source revisions");
		Require(!cache.Update(fileId, 91, sourcePath, MakeRevision(123456789, 128, 42)),
			"file size changes must not affect timestamp-based source revisions");
		Require(cache.Update(fileId, 91, sourcePath + ".moved", MakeRevision(123456789, 128, 42)),
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
		Require(!cache.IsExpired(&info),
			"a same-timestamp same-size edit remains current without content hashing");
		Require(!cache.Update(&info),
			"a same-timestamp source revision should remain unchanged");
		const std::time_t importedSourceTime = info.GetAssetImportTime();

		std::filesystem::last_write_time(metadataPath, initialTime + std::chrono::seconds(4));
		Require(!cache.IsExpired(&info),
			"metadata changes must not participate in persisted asset cache expiration");
		Require(info.IsMetaExpired(),
			"a newer metadata file must remain detectable through runtime AssetInfo state");
		Require(!cache.Update(&info),
			"an unloaded metadata change must not advance the persisted lazy index");
		info.SetProcessingTimes(
			importedSourceTime,
			info.GetMetaLastModificationTime());
		Require(!info.IsMetaExpired(),
			"advancing runtime metadata load time should acknowledge the loaded metadata");
		Require(cache.Update(&info),
			"the loaded metadata revision should advance the persisted v1 lazy index");

		std::filesystem::remove(metadataPath);
		Require(info.IsMetaExpired(), "a missing metadata file should expire runtime AssetInfo state");
		Require(!cache.IsExpired(&info),
			"missing metadata must not be represented as a persisted cache timestamp or path");
		Require(!cache.Update(&info),
			"missing metadata must not advance or remove the last loaded v1 index state");
		Require(!cache.IsExpired(&info),
			"runtime metadata validity must remain independent from persisted source import state");
	}

	void TestRemovingFailedProcessingWatermarkForcesRetry()
	{
		TestAssetCache cache;
		const FileId fileId = MakeFileId("{ASSET-CACHE-FAILED-PROCESSING}");
		TempDirectory directory("failed-processing-retry");
		const std::filesystem::path sourcePath =
			directory.Path("Content/Retry.glb");
		const std::filesystem::path metadataPath =
			directory.Path("Content/Retry.glb.asset");
		WriteFile(sourcePath, "model-source");
		WriteFile(metadataPath, "metadata");

		FileRevision sourceRevision;
		Require(
			Utils::TryGetFileRevision(
				sourcePath.string(),
				sourceRevision),
			"failed-processing source revision fixture must be readable");
		Require(
			cache.Update(
				fileId,
				123,
				sourcePath.string(),
				sourceRevision),
			"a successful processing watermark must initially populate the cache");

		TestAssetInfo info;
		info.Configure(fileId, sourcePath, metadataPath);
		info.SetProcessingTimes(0, 0);
		Require(
			cache.RestoreAssetImportTime(&info, sourceRevision),
			"the previous successful processing watermark must restore before failure");

		cache.Remove(fileId);
		Require(
			!cache.Contains(fileId),
			"a failed processing attempt must invalidate the persisted source watermark");
		Require(
			!cache.RestoreAssetImportTime(&info, sourceRevision),
			"an invalidated watermark must not suppress processing after restart");
		Require(
			cache.IsExpired(&info),
			"the unchanged source must remain expired until processing succeeds again");
	}

	void TestInvalidatedProcessingWatermarkPersistsAcrossCacheInstances()
	{
		TempDirectory directory("persisted-failed-processing-retry");
		const Workspace::WorkspaceContext workspaceContext =
			CreateWorkspaceContext(directory);
		const FileId fileId =
			MakeFileId("{ASSET-CACHE-PERSISTED-FAILED-PROCESSING}");
		const std::filesystem::path sourcePath =
			workspaceContext.GetContent() / "Retry.glb";
		const std::filesystem::path metadataPath =
			workspaceContext.GetContent() / "Retry.glb.asset";
		WriteFile(sourcePath, "model-source");
		WriteFile(metadataPath, "metadata");

		TestAssetInfo info;
		info.Configure(fileId, sourcePath, metadataPath);
		info.SetProcessingTimes(
			info.GetAssetLastModificationTime(),
			info.GetMetaLastModificationTime());

		AssetCache seededCache;
		seededCache.Initialize(workspaceContext);
		Require(
			seededCache.Update(&info),
			"the successful source watermark should populate the configured cache");
		Require(
			seededCache.SaveCache(),
			"the successful source watermark should persist");

		AssetCache loadedCache;
		loadedCache.Initialize(workspaceContext);
		Require(
			loadedCache.Contains(fileId),
			"a second cache instance should load the successful source watermark");

		AssetRegistry registry(workspaceContext);
		const AssetRegistry::AssetProcessingToken token =
			registry.BeginAssetProcessing(&info);
		Require(
			static_cast<bool>(token),
			"the persisted source should begin retry-safe processing");
		registry.CompleteAssetProcessing(token, false);

		AssetCache restartedCache;
		restartedCache.Initialize(workspaceContext);
		Require(
			!restartedCache.Contains(fileId),
			"a cache instance created after invalidation must not restore the source watermark");
		Require(
			restartedCache.IsExpired(&info),
			"the unchanged source should require retry after persisted invalidation");
	}

	void TestAssetProcessingSuccessAndStaleCompletionContract()
	{
		TempDirectory directory("asset-processing-completion");
		const Workspace::WorkspaceContext workspaceContext =
			CreateWorkspaceContext(directory);
		const FileId fileId =
			MakeFileId("{ASSET-PROCESSING-COMPLETION}");
		const std::filesystem::path sourcePath =
			workspaceContext.GetContent() / "Completion.raw";
		const std::filesystem::path metadataPath =
			workspaceContext.GetContent() / "Completion.raw.asset";
		WriteFile(sourcePath, "source");
		WriteFile(metadataPath, "metadata");

		TestAssetInfo info;
		info.Configure(fileId, sourcePath, metadataPath);
		AssetRegistry registry(workspaceContext);

		const AssetRegistry::AssetProcessingToken first =
			registry.BeginAssetProcessing(&info);
		Require(
			static_cast<bool>(first),
			"a readable source should begin processing");
		registry.CompleteAssetProcessing(first, true);
		Require(
			!registry.IsAssetExpired(&info),
			"a successful completion should acknowledge its exact source revision");

		const AssetRegistry::AssetProcessingToken stale =
			registry.BeginAssetProcessing(&info);
		const AssetRegistry::AssetProcessingToken current =
			registry.BeginAssetProcessing(&info);
		Require(
			static_cast<bool>(stale) && static_cast<bool>(current) &&
				stale.m_generation != current.m_generation,
			"consecutive processing attempts should receive distinct generations");
		registry.CompleteAssetProcessing(stale, true);
		Require(
			registry.IsAssetExpired(&info),
			"a stale completion must not acknowledge the current processing attempt");
		registry.CompleteAssetProcessing(current, true);
		Require(
			!registry.IsAssetExpired(&info),
			"the current successful completion should acknowledge its source revision");
	}

	void TestTargetedAssetUpdateScopeAndFailureContract()
	{
		TempDirectory directory("targeted-asset-update");
		const Workspace::WorkspaceContext workspaceContext =
			CreateWorkspaceContext(directory);
		WriteTargetedUpdateFixture(workspaceContext);

		AssetRegistry registry(workspaceContext);
		TargetedUpdateAssetInfoHandler handler;
		RecordingTargetedUpdateListener listener;
		handler.Subscribe(&listener);
		RegisterTargetedUpdateHandler(registry, handler);
		Require(
			registry.ScanContentFolder() &&
				registry.CompleteScanProcessing(),
			"the targeted update fixture should load");

		const FileId primaryId =
			MakeFileId("{TARGETED-UPDATE-PRIMARY}");
		const FileId secondaryId =
			MakeFileId("{TARGETED-UPDATE-SECONDARY}");
		auto* primaryInfo = registry.GetAssetInfoPtr<
			TargetedUpdateAssetInfo*>(primaryId);
		auto* secondaryInfo = registry.GetAssetInfoPtr<
			TargetedUpdateAssetInfo*>(secondaryId);
		Require(
			primaryInfo != nullptr && secondaryInfo != nullptr,
			"both AssetInfos sharing one source should be registered");

		listener.Clear();
		RewriteFileWithNewRevision(
			workspaceContext.GetContent() / "Shared.raw.asset",
			"fileId: '{TARGETED-UPDATE-PRIMARY}'\n"
			"filename: Shared.raw\n"
			"testValue: 11\n");
		Require(
			registry.UpdateAsset(primaryId),
			"a valid metadata-only targeted update should succeed");
		Require(
			listener.m_updatedFileIds ==
				std::vector<FileId>{ primaryId } &&
				listener.m_expirationFlags ==
					std::vector<bool>{ true },
			"metadata-only updates must notify exactly the requested AssetInfo");
		Require(
			primaryInfo->GetTestValue() == 11 &&
				secondaryInfo->GetTestValue() == 2,
			"metadata-only updates must not mutate a sibling AssetInfo");

		listener.Clear();
		Require(
			registry.UpdateAsset(primaryId) &&
				listener.m_updatedFileIds.empty(),
			"an already-current targeted asset should be a successful no-op");

		RewriteFileWithNewRevision(
			workspaceContext.GetContent() / "Shared.raw",
			"shared-source-v2");
		Require(
			registry.UpdateAsset(primaryId),
			"a shared source update should succeed for its whole AssetInfo family");
		Require(
			listener.m_updatedFileIds ==
				std::vector<FileId>{ primaryId, secondaryId } &&
				listener.m_expirationFlags ==
					std::vector<bool>{ true, true },
			"a changed source must notify every AssetInfo that references it");
		Require(
			!primaryInfo->IsAssetExpired() &&
				!secondaryInfo->IsAssetExpired() &&
				!registry.IsAssetExpired(primaryInfo) &&
				!registry.IsAssetExpired(secondaryInfo),
			"the shared source family should commit one current revision");

		listener.Clear();
		RewriteFileWithNewRevision(
			workspaceContext.GetContent() / "Shared.raw2.asset",
			"fileId: '{TARGETED-UPDATE-SECONDARY}'\n"
			"filename: Shared.raw\n"
			"testValue: invalid\n");
		Require(
			!registry.UpdateAsset(secondaryId),
			"an invalid targeted metadata reload must report failure");
		Require(
			secondaryInfo->GetTestValue() == 2 &&
				listener.m_updatedFileIds.empty(),
			"a rejected targeted reload must preserve the live AssetInfo");
		Require(
			!registry.UpdateAsset(
				MakeFileId("{TARGETED-UPDATE-UNKNOWN}")),
			"an unregistered FileId must report targeted update failure");
	}

	void TestTargetedAssetUpdateCoalescesCurrentProcessing()
	{
		TempDirectory directory("targeted-processing-coalescing");
		const Workspace::WorkspaceContext workspaceContext =
			CreateWorkspaceContext(directory);
		WriteTargetedUpdateFixture(workspaceContext);

		AssetRegistry registry(workspaceContext);
		TargetedUpdateAssetInfoHandler handler;
		RecordingTargetedUpdateListener listener;
		handler.Subscribe(&listener);
		RegisterTargetedUpdateHandler(registry, handler);
		Require(
			registry.ScanContentFolder() &&
				registry.CompleteScanProcessing(),
			"the targeted processing fixture should load");

		const FileId primaryId =
			MakeFileId("{TARGETED-UPDATE-PRIMARY}");
		AssetInfoPtr primaryInfo = registry.GetAssetInfoPtr(primaryId);
		Require(
			primaryInfo != nullptr,
			"the targeted processing fixture should expose its primary AssetInfo");
		listener.Clear();

		const AssetRegistry::AssetProcessingToken activeToken =
			registry.BeginAssetProcessing(primaryInfo);
		Require(
			static_cast<bool>(activeToken),
			"the targeted asset should begin asynchronous processing");
		Require(
			registry.UpdateAsset(primaryId) &&
				listener.m_updatedFileIds.empty(),
			"a duplicate targeted request must coalesce with current processing");

		registry.CompleteAssetProcessing(activeToken, false);
		AssetRegistry::AssetProcessingToken retryToken;
		listener.m_onUpdate =
			[&](AssetInfoPtr assetInfo, bool bWasExpired)
			{
				Require(
					bWasExpired,
					"a rejected processing attempt must remain expired for retry");
				retryToken = registry.BeginAssetProcessing(assetInfo);
				Require(
					static_cast<bool>(retryToken),
					"the rejected processing attempt should start a new generation");
				registry.CompleteAssetProcessing(retryToken, true);
			};
		Require(
			registry.UpdateAsset(primaryId),
			"a rejected processing attempt should be retried by a targeted update");
		Require(
			retryToken &&
				retryToken.m_generation != activeToken.m_generation &&
				listener.m_updatedFileIds ==
					std::vector<FileId>{ primaryId },
			"the retry must publish one new processing generation");
	}

	void TestScanSourceRevisionCacheIsPhysicalAndPerScan()
	{
		TempDirectory directory("scan-source-revision-cache");
		const std::filesystem::path engineSource =
			directory.Path("Engine/Content/Shared.glb");
		const std::filesystem::path workspaceSource =
			directory.Path("Workspace/Content/Shared.glb");
		const std::filesystem::path missingSource =
			directory.Path("Workspace/Content/Missing.glb");
		WriteFile(engineSource, "engine-source");
		WriteFile(workspaceSource, "workspace-source-v1");
		std::error_code timestampError;
		std::filesystem::last_write_time(
			workspaceSource,
			std::filesystem::last_write_time(engineSource) + std::chrono::seconds(2),
			timestampError);
		Require(
			!timestampError,
			"the physical source fixture must use distinct file revisions");

		AssetScanSourceRevisionCache scanSnapshot;
		FileRevision engineRevision;
		FileRevision workspaceRevision;
		FileRevision memoizedWorkspaceRevision;
		FileRevision missingRevision;
		Require(
			scanSnapshot.TryGet(engineSource.generic_string(), engineRevision) &&
				scanSnapshot.TryGet(workspaceSource.generic_string(), workspaceRevision),
			"one scan should capture both physical mount sources");
		Require(
			engineRevision != workspaceRevision,
			"equal virtual paths in different mounts must keep independent physical revisions");
		Require(
			!scanSnapshot.TryGet(missingSource.generic_string(), missingRevision),
			"a missing source should be memoized as a failed scan observation");

		RewriteFileWithNewRevision(
			workspaceSource,
			"workspace-source-v2-with-another-size");
		WriteFile(missingSource, "appeared-during-scan");
		const std::filesystem::path equivalentWorkspacePath =
			workspaceSource.parent_path() / "." / workspaceSource.filename();
		Require(
			scanSnapshot.TryGet(
				equivalentWorkspacePath.generic_string(),
				memoizedWorkspaceRevision) &&
				memoizedWorkspaceRevision == workspaceRevision,
			"one scan should reuse its first source revision for equivalent physical paths");
		Require(
			!scanSnapshot.TryGet(missingSource.generic_string(), missingRevision),
			"a source that appears mid-scan should remain absent from that scan snapshot");
		std::string changedPath;
		Require(
			!scanSnapshot.ValidateAll(changedPath) &&
				!changedPath.empty(),
			"commit validation must reject a source snapshot changed during staging");

		AssetScanSourceRevisionCache nextScanSnapshot;
		FileRevision nextWorkspaceRevision;
		Require(
			nextScanSnapshot.TryGet(
				workspaceSource.generic_string(),
				nextWorkspaceRevision) &&
				nextWorkspaceRevision != workspaceRevision,
			"a new scan must observe a changed physical source revision");
		Require(
			nextScanSnapshot.TryGet(missingSource.generic_string(), missingRevision),
			"a new scan must retry a source that was missing from the previous snapshot");
	}

	void TestPostCallbackSourceMismatchInvalidatesPriorWatermark()
	{
		TempDirectory directory("post-callback-source-mismatch");
		const Workspace::WorkspaceContext workspaceContext =
			CreateWorkspaceContext(directory);
		WriteScanAssetFixture(workspaceContext);
		const FileId fileId =
			MakeFileId("{ASSET-CACHE-IMPORT-CONTRACT}");
		const std::filesystem::path sourcePath =
			workspaceContext.GetContent() / "Retry.raw";

		{
			AssetRegistry registry(workspaceContext);
			TestAssetInfoHandler handler;
			RegisterRawHandler(registry, handler);
			Require(
				registry.ScanContentFolder(),
				"the initial source watermark should commit");
		}

		AssetCache initialCache;
		initialCache.Initialize(workspaceContext);
		Require(
			initialCache.Contains(fileId),
			"the initial scan should persist its source watermark");

		RewriteFileWithNewRevision(
			sourcePath,
			"source-v2-with-another-size");
		{
			AssetRegistry registry(workspaceContext);
			TestAssetInfoHandler handler;
			RecordingAssetListener listener;
			listener.m_onExpiredUpdate =
				[&](AssetInfoPtr)
				{
					RewriteFileWithNewRevision(
						sourcePath,
						"source-mutated-by-listener");
				};
			handler.Subscribe(&listener);
			RegisterRawHandler(registry, handler);
			Require(
				registry.ScanContentFolder(),
				"the registry generation should commit before the listener mutation is acknowledged");
			Require(
				!listener.m_events.empty() &&
					listener.m_events[0] == "update:true",
				"the changed source should dispatch an expired update");
		}

		AssetCache restartedCache;
		restartedCache.Initialize(workspaceContext);
		Require(
			!restartedCache.Contains(fileId),
			"a post-callback source mismatch must remove the prior cache watermark");
	}

	void TestRejectedBeginFailsScanAndResetsOnNextScan()
	{
		TempDirectory directory("rejected-begin-scan");
		const Workspace::WorkspaceContext workspaceContext =
			CreateWorkspaceContext(directory);
		WriteScanAssetFixture(workspaceContext);

		AssetRegistry registry(workspaceContext);
		TestAssetInfoHandler handler;
		RegisterRawHandler(registry, handler);
		RecordingAssetListener listener;
		uint32_t scanIndex = 0;
		listener.m_onExpiredUpdate =
			[&](AssetInfoPtr info)
			{
				if (scanIndex == 0)
				{
					std::error_code removeError;
					std::filesystem::remove(
						info->GetAssetFilepath(),
						removeError);
					Require(
						!removeError,
						"the rejected Begin fixture source should be removable");
					Require(
						!registry.BeginAssetProcessing(info),
						"processing should reject a source removed before revision capture");
					return;
				}

				const AssetRegistry::AssetProcessingToken token =
					registry.BeginAssetProcessing(info);
				Require(
					static_cast<bool>(token),
					"the restored source should begin processing");
				registry.CompleteAssetProcessing(token, true);
			};
		handler.Subscribe(&listener);

		Require(
			registry.ScanContentFolder(),
			"the registry generation should commit before the processing callback is rejected");
		Require(
			!registry.CompleteScanProcessing(),
			"a rejected BeginAssetProcessing during scan must fail scan processing");

		WriteFile(
			workspaceContext.GetContent() / "Retry.raw",
			"source-restored");
		++scanIndex;
		Require(
			registry.ScanContentFolder(),
			"the next scan should accept the restored source");
		Require(
			registry.CompleteScanProcessing(),
			"scan processing failure state must reset for the next successful scan");
	}

	void TestSourceMutationDuringStagingPreservesPreviousGeneration()
	{
		TempDirectory directory("source-mutation-during-staging");
		const Workspace::WorkspaceContext workspaceContext =
			CreateWorkspaceContext(directory);
		WriteScanAssetFixture(workspaceContext);
		const std::filesystem::path sourcePath =
			workspaceContext.GetContent() / "Retry.raw";

		AssetRegistry registry(workspaceContext);
		TestAssetInfoHandler handler;
		RegisterRawHandler(registry, handler);
		Require(
			registry.ScanContentFolder(),
			"the initial registry generation should commit");
		AssetInfoPtr previousInfo = registry.GetAssetInfoPtr(sourcePath.string());
		Require(previousInfo != nullptr,
			"the initial registry generation should expose the fixture asset");

		handler.m_onLoad = [&]()
			{
				RewriteFileWithNewRevision(
					sourcePath,
					"source-mutated-during-staging");
			};
		Require(
			!registry.ScanContentFolder(),
			"a source mutation during staging must reject the new generation");
		Require(
			registry.GetAssetInfoPtr(sourcePath.string()) == previousInfo,
			"a rejected staged generation must preserve the previous asset info");

		Require(
			registry.ScanContentFolder(),
			"the next stable scan should accept the changed source");
		Require(
			registry.GetAssetInfoPtr(sourcePath.string()) != nullptr,
			"the stable retry must publish the asset again");
	}

	void TestMetadataMutationDuringPreCommitWaitPreservesPreviousGeneration()
	{
		TempDirectory directory("metadata-mutation-during-pre-commit-wait");
		const Workspace::WorkspaceContext workspaceContext =
			CreateWorkspaceContext(directory);
		WriteScanAssetFixture(workspaceContext);
		const std::filesystem::path sourcePath =
			workspaceContext.GetContent() / "Retry.raw";
		const std::filesystem::path metadataPath =
			workspaceContext.GetContent() / "Retry.raw.asset";

		bool bMetadataMutatedDuringWait = false;
		Tasks::Scheduler scheduler;
		scheduler.AttachCurrentThreadAsMainThread();
		{
			AssetRegistry registry(workspaceContext, &scheduler);
			TestAssetInfoHandler handler;
			RegisterRawHandler(registry, handler);
			Require(
				registry.ScanContentFolder(),
				"the initial registry generation should commit");
			AssetInfoPtr previousInfo = registry.GetAssetInfoPtr(sourcePath.string());
			Require(previousInfo != nullptr,
				"the initial registry generation should expose the fixture asset");

			handler.m_onLoad = [&]()
				{
					Tasks::ITaskPtr mutationTask =
						TSharedPtr<TestMainThreadTask>::Make(
							[&]()
								{
									bMetadataMutatedDuringWait = true;
									RewriteFileWithNewRevision(
										metadataPath,
										"fileId: '{ASSET-CACHE-IMPORT-CONTRACT}'\n"
										"filename: Retry.raw\n"
										"testValue: 8\n"
										"lateValue: 1\n");
								});
					scheduler.Run(mutationTask);
					Require(!bMetadataMutatedDuringWait,
						"the metadata mutation must remain queued until the pre-commit wait");
				};

			Require(
				!registry.ScanContentFolder(),
				"a metadata mutation during the pre-commit wait must reject the new generation");
			Require(bMetadataMutatedDuringWait,
				"the metadata mutation must execute inside the pre-commit wait");
			Require(
				registry.GetAssetInfoPtr(sourcePath.string()) == previousInfo,
				"a rejected staged generation must preserve the previous asset info");

			Require(
				registry.ScanContentFolder(),
				"the next stable scan should accept the changed metadata");
			Require(
				registry.GetAssetInfoPtr(sourcePath.string()) != nullptr,
				"the stable retry must publish the asset again");
		}
	}

	void TestSynchronousFailedCompletionFailsScan()
	{
		TempDirectory directory("failed-completion-scan");
		const Workspace::WorkspaceContext workspaceContext =
			CreateWorkspaceContext(directory);
		WriteScanAssetFixture(workspaceContext);

		AssetRegistry registry(workspaceContext);
		TestAssetInfoHandler handler;
		RegisterRawHandler(registry, handler);
		RecordingAssetListener listener;
		bool bSucceed = false;
		listener.m_onExpiredUpdate =
			[&](AssetInfoPtr info)
			{
				const AssetRegistry::AssetProcessingToken token =
					registry.BeginAssetProcessing(info);
				Require(
					static_cast<bool>(token),
					"the scan fixture should begin processing");
				registry.CompleteAssetProcessing(token, bSucceed);
			};
		handler.Subscribe(&listener);

		Require(
			registry.ScanContentFolder(),
			"the registry generation should commit before synchronous processing fails");
		Require(
			!registry.CompleteScanProcessing(),
			"a synchronous failed completion must fail scan processing");
		Require(
			registry.CompleteScanProcessing(),
			"the failed scan processing outcome should be consumed exactly once");

		bSucceed = true;
		Require(
			registry.ScanContentFolder(),
			"the retry scan should commit");
		Require(
			registry.CompleteScanProcessing(),
			"a successful retry should clear the previous scan failure outcome");
	}

	void TestSourceMutationRejectsCompletionAndFailsScan()
	{
		TempDirectory directory("source-mutation-scan");
		const Workspace::WorkspaceContext workspaceContext =
			CreateWorkspaceContext(directory);
		WriteScanAssetFixture(workspaceContext);

		AssetRegistry registry(workspaceContext);
		TestAssetInfoHandler handler;
		RegisterRawHandler(registry, handler);
		RecordingAssetListener listener;
		AssetInfoPtr processedInfo = nullptr;
		listener.m_onExpiredUpdate =
			[&](AssetInfoPtr info)
			{
				processedInfo = info;
				const AssetRegistry::AssetProcessingToken token =
					registry.BeginAssetProcessing(info);
				Require(
					static_cast<bool>(token),
					"the source mutation fixture should begin processing");
				RewriteFileWithNewRevision(
					info->GetAssetFilepath(),
					"source-mutated-after-begin");
				registry.CompleteAssetProcessing(token, true);
			};
		handler.Subscribe(&listener);

		Require(
			registry.ScanContentFolder(),
			"the registry generation should commit before source mutation is detected");
		Require(
			!registry.CompleteScanProcessing(),
			"a completion for a changed source revision must fail scan processing");
		Require(
			processedInfo != nullptr &&
				registry.IsAssetExpired(processedInfo),
			"a changed source completion must leave its watermark invalid for retry");

		AssetCache restartedCache;
		restartedCache.Initialize(workspaceContext);
		Require(
			!restartedCache.Contains(
				MakeFileId("{ASSET-CACHE-IMPORT-CONTRACT}")),
			"the rejected changed-source completion must remain invalid after cache reload");
	}

	void TestRuntimeMetadataFieldsAreIgnored()
	{
		const FileId parsedId = MakeFileId("{ASSET-CACHE-STRICT}");
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
			"        contentHash: 3\n"
			"      metadataFilename: Test.mat.asset\n"
			"      metadataRevision:\n"
			"        modificationTimeNanoseconds: 1\n"
			"        fileSize: 2\n"
			"        contentHash: 3\n"
			"      assetInfoType: Sailor::MaterialAssetInfo\n";
		const std::string payloads[] =
		{
			prefix + "      metadataLoadTime: 2\n",
			prefix + "      metadataPath: '/workspace/Content/Test.mat.asset'\n",
			prefix +
				"      metadataLoadTime: 2\n"
				"      metadataPath: '/workspace/Content/Test.mat.asset'\n",
			prefix +
				"      unexpectedField: 3\n"
		};

		for (const std::string& payload : payloads)
		{
			auto destination = MakeCache(
				"{ASSET-CACHE-RETAINED}",
				"/workspace/Content/Retained.mat",
				6);
			std::string diagnostic;
			Require(TestAssetCache::TryDeserializeAssetCachePayload(payload, destination, diagnostic),
				"engine-generated cache payloads should ignore fields unknown to the current reader");
			Require(diagnostic.empty(), "ignored cache fields should not produce diagnostics");
			Require(destination.m_assets.Num() == 1 && destination.m_assets.ContainsKey(parsedId),
				"a valid decoded cache should replace the previous snapshot atomically");
			const std::string serialized = TestAssetCache::SerializeAssetCachePayload(destination);
			Require(serialized.find("metadataLoadTime") == std::string::npos &&
				serialized.find("metadataPath") == std::string::npos &&
				serialized.find("unexpectedField") == std::string::npos,
				"runtime-only and unknown fields should not be written back to the cache");
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
		TestNewFileIdsUseCrossPlatformGuidFormatting();
		TestPayloadRoundTrip();
		TestEmptyPayloadRoundTrip();
		TestPreV1PayloadIsRejected();
		TestV2EnvelopeIsResetInsteadOfMigrated();
		TestLazyScanDefersUnchangedMetadataMaterialization();
		TestLazyScanLoadsOnlyNewSecondaryMetadata();
		TestScanCanonicalizesUuidMetadataIdentity();
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
		TestRemovingFailedProcessingWatermarkForcesRetry();
		TestInvalidatedProcessingWatermarkPersistsAcrossCacheInstances();
		TestAssetProcessingSuccessAndStaleCompletionContract();
		TestTargetedAssetUpdateScopeAndFailureContract();
		TestTargetedAssetUpdateCoalescesCurrentProcessing();
		TestScanSourceRevisionCacheIsPhysicalAndPerScan();
		TestPostCallbackSourceMismatchInvalidatesPriorWatermark();
		TestSourceMutationDuringStagingPreservesPreviousGeneration();
		TestMetadataMutationDuringPreCommitWaitPreservesPreviousGeneration();
		TestRejectedBeginFailsScanAndResetsOnNextScan();
		TestSynchronousFailedCompletionFailsScan();
		TestSourceMutationRejectsCompletionAndFailsScan();
		TestAssetImportCacheAndRuntimeMetadataExpiration();
		TestRuntimeMetadataFieldsAreIgnored();
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
