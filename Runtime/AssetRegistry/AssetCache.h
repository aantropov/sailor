#pragma once
#include <chrono>
#include <ctime>
#include "Containers/ConcurrentMap.h"
#include "AssetRegistry/FileId.h"
#include <mutex>
#include <filesystem>

namespace Sailor
{
	class AssetCache
	{
	public:

		static std::string GetAssetCacheFilepath() { return App::GetWorkspace() + "Cache/AssetCache.yaml"; }

		SAILOR_API void Initialize();
		SAILOR_API void Shutdown();

		SAILOR_API bool GetTimeStamp(const FileId& uid, time_t& outAssetTimestamp) const;

		SAILOR_API void Update(const FileId& id, const time_t& assetTimestamp);
		SAILOR_API void Remove(const FileId& uid);

		SAILOR_API bool Contains(const FileId& uid) const;
		SAILOR_API bool IsExpired(const class AssetInfo* info) const;

		SAILOR_API void LoadCache();
		SAILOR_API void SaveCache(bool bForcely = false);

		SAILOR_API void ClearAll();

	protected:

		class Entry final : IYamlSerializable
		{
		public:

			FileId m_fileId{};
			std::time_t m_assetImportTime{};

			SAILOR_API bool operator==(const Entry& rhs) const { return m_fileId == rhs.m_fileId; }

			SAILOR_API virtual YAML::Node Serialize() const override;
			SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;
		};

		class AssetCacheData final : IYamlSerializable
		{
		public:
			TConcurrentMap<FileId, AssetCache::Entry> m_data{};

			SAILOR_API virtual YAML::Node Serialize() const override;
			SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;
		};

		std::mutex m_saveToCacheMutex;

	private:

		AssetCacheData m_cache;
		bool m_bIsDirty = false;
	};
}
