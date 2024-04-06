#include "AssetCache.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include "Core/YamlSerializable.h"
#include "Containers/ConcurrentMap.h"
#include "AssetRegistry/AssetRegistry.h"

using namespace Sailor;

YAML::Node AssetCache::Entry::Serialize() const
{
	YAML::Node res;
	res["fileId"] = m_fileId;
	res["assetImportTime"] = m_assetImportTime;

	return res;
}

void AssetCache::Entry::Deserialize(const YAML::Node& inData)
{
	m_fileId = inData["fileId"].as<FileId>();
	m_assetImportTime = inData["assetImportTime"].as<std::time_t>();
}

YAML::Node AssetCache::AssetCacheData::Serialize() const
{
	YAML::Node res;
	res["assets"] = m_data;
	return res;
}

void AssetCache::AssetCacheData::Deserialize(const YAML::Node& inData)
{
	m_data = inData["assets"].as<TConcurrentMap<FileId, AssetCache::Entry>>();
}

void AssetCache::Initialize()
{
	SAILOR_PROFILE_FUNCTION();

	std::filesystem::create_directory(AssetRegistry::GetCacheFolder());

	auto assetCacheFilePath = std::filesystem::path(GetAssetCacheFilepath());
	if (!std::filesystem::exists(GetAssetCacheFilepath()))
	{
		YAML::Node outData;
		outData["assetCache"] = m_cache.Serialize();

		std::ofstream assetFile(assetCacheFilePath);
		assetFile << outData;
		assetFile.close();
	}

	LoadCache();
}

void AssetCache::Shutdown()
{
	SaveCache();
}

void AssetCache::SaveCache(bool bForcely)
{
	SAILOR_PROFILE_FUNCTION();

	if (bForcely || m_bIsDirty)
	{
		YAML::Node outData;
		outData["assetCache"] = m_cache.Serialize();

		std::ofstream assetFile(GetAssetCacheFilepath());
		assetFile << outData;
		assetFile.close();

		m_bIsDirty = false;
	}
}

void AssetCache::LoadCache()
{
	SAILOR_PROFILE_FUNCTION();

	std::string yaml;
	AssetRegistry::ReadAllTextFile(GetAssetCacheFilepath(), yaml);

	YAML::Node yamlNode = YAML::Load(yaml);
	m_cache.Deserialize(yamlNode["assetCache"]);

	m_bIsDirty = false;
}

void AssetCache::ClearAll()
{
	std::lock_guard<std::mutex> lk(m_saveToCacheMutex);
	std::filesystem::remove_all(GetAssetCacheFilepath());
}

bool AssetCache::Contains(const FileId& uid) const { return m_cache.m_data.ContainsKey(uid); }

bool AssetCache::GetTimeStamp(const FileId& uid, time_t& outAssetTimestamp) const
{
	if (Contains(uid))
	{
		outAssetTimestamp = m_cache.m_data[uid].m_assetImportTime;
		return true;
	}
	return false;
}

void AssetCache::Update(const FileId& id, const time_t& assetTimestamp)
{
	auto& entry = m_cache.m_data.At_Lock(id);

	m_bIsDirty |= entry.m_assetImportTime < assetTimestamp;
	entry.m_assetImportTime = assetTimestamp;

	m_cache.m_data.Unlock(id);
}

void AssetCache::Remove(const FileId& uid)
{
}

bool AssetCache::IsExpired(const AssetInfo* info) const
{
	return m_cache.m_data[info->GetFileId()].m_assetImportTime < info->GetAssetLastModificationTime();
}