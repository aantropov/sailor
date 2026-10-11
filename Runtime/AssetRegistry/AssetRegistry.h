#pragma once
#include <string>
#include <string_view>
#include <fstream>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <vector>
#include <type_traits>
#include "Sailor.h"
#include "Containers/Containers.h"
#include "AssetRegistry/FileId.h"
#include "Core/Submodule.h"
#include "AssetRegistry/AssetInfo.h"
#include "Core/Singleton.hpp"
#include "Engine/Object.h"
#include "AssetRegistry/AssetCache.h"
#include "Tasks/Tasks.h"
#include "Workspace/WorkspacePathEncoding.h"

namespace Sailor
{
	class AssetInfo;
	using AssetInfoPtr = AssetInfo*;
	SAILOR_SHARED_API extern bool g_bUseLazyAssetInfoLoading;

	enum class EAssetType;

	class IAssetRegistryContentListener
	{
	public:
		virtual ~IAssetRegistryContentListener() = default;
		virtual void OnAssetScanStarted() {}
		virtual Tasks::TaskPtr<bool> OnAssetScanFinished() { return Tasks::TaskPtr<bool>::Make(true); }
		virtual Tasks::TaskPtr<bool> OnEffectiveContentChanged(
			const std::string& virtualPath) = 0;
	};

	class AssetRegistry final : public TSubmodule<AssetRegistry>
	{
	public:
		struct AssetReadLocation final
		{
			std::filesystem::path m_physicalPath;
			std::string m_virtualPath;
			EAssetMountKind m_mountKind = EAssetMountKind::Engine;
			bool m_bWritable = false;
			FileRevision m_revision{};
		};

		struct AssetProcessingToken final
		{
			FileId m_fileId{};
			FileRevision m_sourceRevision{};
			std::string m_sourcePath;
			std::time_t m_assetImportTime{};
			uint64_t m_generation{};

			explicit operator bool() const noexcept
			{
				return static_cast<bool>(m_fileId) &&
					m_sourceRevision.m_bIsValid &&
					!m_sourcePath.empty() &&
					m_assetImportTime > 0 &&
					m_generation != 0;
			}

			bool Matches(const AssetProcessingToken& rhs) const noexcept
			{
				return m_fileId == rhs.m_fileId &&
					m_sourceRevision == rhs.m_sourceRevision &&
					m_sourcePath == rhs.m_sourcePath &&
					m_assetImportTime == rhs.m_assetImportTime &&
					m_generation == rhs.m_generation;
			}
		};

		SAILOR_API static std::string GetContentFolder();
		SAILOR_API static std::string GetWorkspaceContentFolder();
		SAILOR_API static std::string GetEngineContentFolder();
		SAILOR_API static std::string GetCacheFolder();

		static constexpr const char* MetaFileExtension = "asset";

		SAILOR_API AssetRegistry();
		SAILOR_API explicit AssetRegistry(
			const Workspace::WorkspaceContext& workspaceContext);
		SAILOR_API AssetRegistry(
			const Workspace::WorkspaceContext& workspaceContext,
			Tasks::Scheduler* scheduler);
		SAILOR_API virtual ~AssetRegistry() override;
		const Workspace::WorkspaceContext& GetWorkspaceContext() const { return m_workspaceContext; }

		template<typename TBinaryType, typename TFilepath>
		static bool ReadBinaryFile(const TFilepath& filename, TVector<TBinaryType>& buffer)
		{
			std::ifstream file(NativeFilePath(filename), std::ios::ate | std::ios::binary);
			file.unsetf(std::ios::skipws);

			if (!file.is_open())
			{
				return false;
			}

			size_t fileSize = (size_t)file.tellg();
			buffer.Clear();

			size_t mod = fileSize % sizeof(TBinaryType);
			size_t size = fileSize / sizeof(TBinaryType) + (mod ? 1 : 0);
			buffer.Resize(size);

			//buffer.resize(fileSize / sizeof(T));

			file.seekg(0, std::ios::beg);
			file.read(reinterpret_cast<char*>(buffer.GetData()), fileSize);

			file.close();
			return true;
		}

		template<typename TTextType, typename TFilepath>
		static bool ReadTextFile(const TFilepath& filename, TTextType& outText)
		{
			const auto& path = NativeFilePath(filename);
			std::ifstream file(path, std::ios::in | std::ios::ate);
			if (!file.is_open())
			{
				return false;
			}

			file.seekg(0, std::ios::beg);

			std::stringstream buffer;
			buffer << file.rdbuf();
			outText = buffer.str();

			file.close();
#if defined(SAILOR_FILE_IO_TEST_HOOKS)
			NotifyTextReadForTests(path);
#endif
			return true;
		}

		template<typename TBinaryType, typename TFilepath>
		static void WriteBinaryFile(const TFilepath& filename, const TVector<TBinaryType>& buffer)
		{
			std::ofstream file(NativeFilePath(filename), std::ofstream::binary);
			file.write(reinterpret_cast<const char*>(buffer.GetData()), buffer.Num() * sizeof(buffer[0]));
			file.close();
		}

		template<typename TStringType, typename TFilepath>
		static void WriteTextFile(const TFilepath& filename, const TStringType& text)
		{
			std::ofstream file(NativeFilePath(filename));
			file << text;
			file.close();
		}

		SAILOR_API static bool ReadAllTextFile(const std::string& filename, std::string& text);
		SAILOR_API bool ResolveContentFile(
			std::string_view virtualPath,
			AssetReadLocation& outLocation) const;
		SAILOR_API bool ReadContentText(std::string_view virtualPath, std::string& outText) const;
		SAILOR_API bool GetContentFileModificationTime(
			std::string_view virtualPath,
			std::time_t& outTimestamp) const;
		SAILOR_API bool ResolveWorkspaceContentPathForWrite(
			std::string_view virtualPath,
			std::filesystem::path& outPath) const;

		template<typename TBinaryType>
		bool ReadContentBinary(std::string_view virtualPath, TVector<TBinaryType>& outBuffer) const
		{
			AssetReadLocation location;
			return ResolveContentFile(virtualPath, location) &&
				ReadBinaryFile(location.m_physicalPath, outBuffer);
		}

		SAILOR_API bool ScanContentFolder();
		SAILOR_API bool UpdateAsset(const FileId& fileId, bool bReimport = false);
		SAILOR_API bool UpdateAsset(const FileId& fileId,
			TVector<AssetInfoPtr>& outAffectedAssets, bool bReimport = false);
		// Check the same update's assets after the importer tasks have finished.
		SAILOR_API bool CompleteAssetUpdate(const TVector<AssetInfoPtr>& affectedAssets) const;
		// Existing engine assets reload synchronously on Main. Other queues return
		// the current ID and request an update on Main without waiting for it.
		SAILOR_API const FileId& GetOrLoadFile(std::string_view filepath);

		template<typename TAssetInfoPtr = AssetInfoPtr>
		TAssetInfoPtr GetAssetInfoPtr(FileId uid) const
		{
			return 	dynamic_cast<TAssetInfoPtr>(GetAssetInfoPtr_Internal(uid));
		}

		template<typename TAssetInfoPtr = AssetInfoPtr>
		TAssetInfoPtr GetAssetInfoPtr(std::string_view assetFilepath) const
		{
			return 	dynamic_cast<TAssetInfoPtr>(GetAssetInfoPtr_Internal(assetFilepath));
		}

		template<class TAssetInfo>
		void GetAllAssetInfos(TVector<FileId>& outAssetInfos) const
		{
			outAssetInfos.Clear();
			TSet<FileId> resultIds;
			for (const auto& assetInfo : m_loadedAssetInfo)
			{
				if (dynamic_cast<TAssetInfo*>(*assetInfo.m_second))
				{
					resultIds.Insert(assetInfo.m_first);
				}
			}

			if (g_bUseLazyAssetInfoLoading)
			{
				TAssetInfo assetInfoTypeProbe;
				TVector<FileId> lazyIds;
				GetLazyAssetInfoIds(assetInfoTypeProbe.GetAssetInfoType(), lazyIds);
				for (const FileId& lazyId : lazyIds)
				{
					resultIds.Insert(lazyId);
				}
			}

			outAssetInfos.Reserve(resultIds.Num());
			for (const FileId& fileId : resultIds)
			{
				outAssetInfos.Add(fileId);
			}
		}

		SAILOR_API bool RegisterAssetInfoHandler(const TVector<std::string>& supportedExtensions, class IAssetInfoHandler* pAssetInfoHandler);
		SAILOR_API void SubscribeContentChanges(IAssetRegistryContentListener* listener);
		SAILOR_API void UnsubscribeContentChanges(IAssetRegistryContentListener* listener);
		SAILOR_API static std::string GetMetaFilePath(std::string_view assetFilePath);

		SAILOR_API bool IsAssetExpired(const AssetInfoPtr info) const;
		SAILOR_API void CacheAsset(const AssetInfoPtr info);
		SAILOR_API AssetProcessingToken BeginAssetProcessing(AssetInfoPtr info);
		SAILOR_API void CompleteAssetProcessing(
			const AssetProcessingToken& token,
			bool bSucceeded);
		SAILOR_API void TrackScanProcessingTask(
			const Tasks::TaskPtr<bool>& processingTask);
		SAILOR_API bool CompleteScanProcessing();

#if defined(SAILOR_FILE_IO_TEST_HOOKS)
		uint64_t TakeManifestWritesForTests() { return m_assetCache.TakeManifestWriteCountForTests(); }
		using TextReadObserver = std::function<void(const std::filesystem::path&)>;
		SAILOR_API static TextReadObserver ExchangeTextReadObserverForTests(TextReadObserver observer);
#endif

		template<typename T>
		TObjectPtr<T> LoadAssetFromFile(const FileId& id, bool bImmediate = true)
		{
			TObjectPtr<Object> out;
			if (const auto& info = GetAssetInfoPtr(id))
			{
				out = LoadAsset(info->GetHandler(), id, bImmediate);
			}

			return out.DynamicCast<T>();
		}

	protected:

#if defined(SAILOR_FILE_IO_TEST_HOOKS)
		SAILOR_API static void NotifyTextReadForTests(const std::filesystem::path& path);
#endif

		SAILOR_API TObjectPtr<Object> LoadAsset(IAssetInfoHandler* assetInfoHandler, const FileId& id, bool bImmediate);

		SAILOR_API const FileId& LoadFile(std::string_view filepath);
		void RequestAssetUpdate(const FileId& fileId);

		SAILOR_API AssetInfoPtr GetAssetInfoPtr_Internal(FileId uid) const;
		SAILOR_API AssetInfoPtr GetAssetInfoPtr_Internal(std::string_view assetFilepath) const;
		bool ScanContentFolderLazy();
		bool BeginScanProcessing(const TVector<FileId>& changedAssets);
		void FinishScanProcessing();
		bool CommitScanProcessing();
		AssetInfoPtr MaterializeLazyAssetInfo(FileId uid) const;
		void GetLazyAssetInfoIds(
			std::string_view assetInfoType,
			TVector<FileId>& outIds) const;
		void GetAssetInfoIdsByTypeAndSource(
			std::string_view assetInfoType,
			std::string_view sourcePath,
			TVector<FileId>& outIds) const;
		bool RestoreAssetImportTime(
			AssetInfoPtr info,
			const FileRevision& sourceRevision);
		bool ResolveDirectLoadPath(
			std::string_view requestedPath,
			AssetReadLocation& outLocation) const;
		IAssetInfoHandler* GetAssetInfoHandler(std::string_view extension) const;
		IAssetInfoHandler* GetAssetInfoHandler(const AssetInfo& info) const;
		IAssetInfoHandler* GetAssetInfoHandler(
			std::string_view extension,
			std::string_view assetInfoType,
			bool bPrimary) const;

		Workspace::WorkspaceContext m_workspaceContext;
		TMap<FileId, AssetInfoPtr> m_loadedAssetInfo;
		TMap<std::string, FileId> m_fileIds;
		TMap<std::string, FileId> m_physicalFileIds;
		TMap<std::string, class IAssetInfoHandler*> m_assetInfoHandlers;
		TVector<AssetMountDescriptor> m_contentMounts;
		TMap<std::string, AssetReadLocation> m_contentFileWinners;
		TVector<IAssetRegistryContentListener*> m_contentListeners;
		struct LazyAssetInfoRecord final
		{
			std::string m_sourcePath;
			FileRevision m_sourceRevision{};
			std::string m_metadataFilename;
			FileRevision m_metadataRevision{};
			std::string m_assetInfoType;
		};
		mutable std::recursive_mutex m_lazyAssetInfoMutex;
		mutable TMap<FileId, LazyAssetInfoRecord> m_lazyAssetInfos;
		struct AssetProcessingState final
		{
			AssetProcessingToken m_token;
			std::string m_metadataFilename;
			std::string m_assetInfoType;
			bool m_bRejected = false;
			FileRevision m_completedMetadataRevision{};
		};
		std::mutex m_assetProcessingMutex;
		TMap<FileId, AssetProcessingState> m_assetProcessingStates;
		TSet<FileId> m_pendingAssetUpdates;
		TVector<Tasks::TaskPtr<bool>> m_scanProcessingTasks;
		TSet<FileId> m_scanInvalidatedAssets;
		bool m_bCollectScanProcessingTasks = false;
		bool m_bScanProcessingActive = false;
		bool m_bScanProcessingFailed = false;
		Tasks::Scheduler* m_scheduler = nullptr;

		AssetCache m_assetCache;

	private:

		template<typename TFilepath>
		static decltype(auto) NativeFilePath(const TFilepath& filename)
		{
			if constexpr (std::is_convertible_v<const TFilepath&, std::string_view>)
			{
				return Workspace::PathFromUtf8(filename);
			}
			else
			{
				return (filename);
			}
		}

		FileId RegisterGeneratedSecondaryAssetInfo(
			const std::filesystem::path& metadataPath);
		// Unclaimed IDs keep the proposed path; owned IDs retain their existing sidecar location.
		bool CanReuseSecondaryAssetId(const FileId& fileId,
			const std::string& assetInfoType,
			const std::filesystem::path& sourcePath,
			std::filesystem::path& inOutMetadataPath) const;

		friend class IAssetInfoHandler;
		friend class ModelImporter;
	};
}
