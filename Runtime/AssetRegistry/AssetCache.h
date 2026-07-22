#pragma once
#include <cstdint>
#include <ctime>
#include "Containers/ConcurrentMap.h"
#include "Containers/Set.h"
#include "AssetRegistry/FileId.h"
#include "Workspace/WorkspaceCacheContract.h"
#include <mutex>
#include <filesystem>
#include <string>

namespace Sailor
{
	class AssetCache
	{
	public:
		SAILOR_API static std::string GetAssetCacheFilepath();

		SAILOR_API void Initialize();
		SAILOR_API void Shutdown();

		SAILOR_API bool Update(const class AssetInfo* info);
		SAILOR_API void Remove(const FileId& uid);

		SAILOR_API bool Contains(const FileId& uid) const;
		SAILOR_API bool IsExpired(const class AssetInfo* info) const;

		SAILOR_API void LoadCache();
		SAILOR_API void SaveCache(bool bForcely = false);

		SAILOR_API void ClearAll();
		SAILOR_API Workspace::WorkspaceCacheLoadResult GetLastLoadResult() const;
		SAILOR_API std::string GetLastSaveDiagnostic() const;
		SAILOR_API bool IsDirty() const;

	protected:

		class AssetCacheData final : IYamlSerializable
		{
		public:

			class Entry final : IYamlSerializable
			{
			public:

				FileId m_fileId{};
				std::time_t m_assetImportTime{};
				std::string m_sourcePath;

				SAILOR_API bool operator==(const Entry& rhs) const
				{
					return m_fileId == rhs.m_fileId &&
						m_assetImportTime == rhs.m_assetImportTime &&
						m_sourcePath == rhs.m_sourcePath;
				}

				SAILOR_API virtual YAML::Node Serialize() const override;
				SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;
			};

			TConcurrentMap<FileId, AssetCache::AssetCacheData::Entry> m_data{};

			SAILOR_API virtual YAML::Node Serialize() const override;
			SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;

			static bool TryDeserialize(
				const YAML::Node& inData,
				AssetCacheData& outData,
				std::string& outDiagnostic) noexcept;
		};

		SAILOR_API static bool ShouldResetCacheFile(
			Workspace::EWorkspaceCacheLoadStatus status) noexcept;
		SAILOR_API static bool ShouldWriteCacheFile(
			bool bForcely,
			bool bIsDirty,
			bool bPreserveStorageAfterLoadFailure) noexcept;
		SAILOR_API bool Update(
			const FileId& id,
			std::time_t assetImportTime,
			const std::string& sourcePath);
		SAILOR_API bool RestoreAssetImportTime(class AssetInfo* info) const;
		SAILOR_API bool Prune(const TSet<FileId>& liveAssetIds);

		SAILOR_API static std::string SerializeAssetCachePayload(const AssetCacheData& cache);
		SAILOR_API static bool TryDeserializeAssetCachePayload(
			const std::string& payload,
			AssetCacheData& outData,
			std::string& outDiagnostic) noexcept;
		bool WriteCacheLocked(std::string& outDiagnostic) noexcept;
		void ResetInvalidCacheLocked(Workspace::WorkspaceCacheLoadResult loadResult);

		mutable std::mutex m_cacheMutex;

	private:
		AssetCacheData m_cache;
		bool m_bIsDirty = false;
		bool m_bPreserveStorageAfterLoadFailure = false;
		Workspace::WorkspaceCacheLoadResult m_lastLoadResult{};
		std::string m_lastSaveDiagnostic;

		friend class AssetRegistry;
	};
}
