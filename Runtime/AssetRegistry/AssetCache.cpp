#include "AssetCache.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include "Core/YamlSerializable.h"
#include "Containers/ConcurrentMap.h"
#include "AssetRegistry/AssetRegistry.h"

#include <algorithm>
#include <cctype>

using namespace Sailor;

namespace
{
	std::string NormalizeSourcePath(const std::string& sourcePath)
	{
		std::error_code error;
		std::string result = std::filesystem::weakly_canonical(sourcePath, error).generic_string();
		if (error)
		{
			result = std::filesystem::path(sourcePath).lexically_normal().generic_string();
		}
#if defined(_WIN32)
		std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character)
			{
				return static_cast<char>(std::tolower(character));
			});
#endif
		return result;
	}
}

std::string AssetCache::GetAssetCacheFilepath()
{
	return (std::filesystem::path(AssetRegistry::GetCacheFolder()) / "AssetCache.yaml").string();
}

YAML::Node AssetCache::AssetCacheData::Entry::Serialize() const
{
	YAML::Node res;
	res["fileId"] = m_fileId;
	res["assetImportTime"] = m_assetImportTime;
	res["sourcePath"] = m_sourcePath;

	return res;
}

void AssetCache::AssetCacheData::Entry::Deserialize(const YAML::Node& inData)
{
	m_fileId = inData["fileId"].as<FileId>();
	m_assetImportTime = inData["assetImportTime"].as<std::time_t>();
	m_sourcePath.clear();
	for (const auto& field : inData)
	{
		if (field.first.IsScalar() && field.first.Scalar() == "sourcePath" && field.second.IsScalar())
		{
			m_sourcePath = NormalizeSourcePath(field.second.as<std::string>());
			break;
		}
	}
}

YAML::Node AssetCache::AssetCacheData::Serialize() const
{
	YAML::Node res;
	res["assets"] = m_data;
	return res;
}

void AssetCache::AssetCacheData::Deserialize(const YAML::Node& inData)
{
	m_data = inData["assets"].as<TConcurrentMap<FileId, AssetCache::AssetCacheData::Entry>>();
}

void AssetCache::Initialize()
{
	SAILOR_PROFILE_FUNCTION();

	std::filesystem::create_directories(AssetRegistry::GetCacheFolder());

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

bool AssetCache::GetTimeStamp(
	const FileId& uid,
	const std::string& sourcePath,
	time_t& outAssetTimestamp) const
{
	if (Contains(uid))
	{
		const AssetCacheData::Entry& entry = m_cache.m_data[uid];
		if (entry.m_sourcePath.empty() || entry.m_sourcePath != NormalizeSourcePath(sourcePath))
		{
			return false;
		}
		outAssetTimestamp = entry.m_assetImportTime;
		return true;
	}
	return false;
}

void AssetCache::Update(
	const FileId& id,
	const std::string& sourcePath,
	const time_t& assetTimestamp)
{
	auto& entry = m_cache.m_data.At_Lock(id);
	const std::string normalizedSourcePath = NormalizeSourcePath(sourcePath);

	m_bIsDirty |= entry.m_assetImportTime != assetTimestamp ||
		entry.m_sourcePath != normalizedSourcePath;
	entry.m_fileId = id;
	entry.m_assetImportTime = assetTimestamp;
	entry.m_sourcePath = normalizedSourcePath;

	m_cache.m_data.Unlock(id);
}

void AssetCache::Remove(const FileId& uid)
{
}

bool AssetCache::IsExpired(const AssetInfo* info) const
{
	time_t cachedTimestamp = 0;
	return !GetTimeStamp(info->GetFileId(), info->GetAssetFilepath(), cachedTimestamp) ||
		cachedTimestamp < info->GetAssetLastModificationTime();
}
