#include "ShaderCache.h"
#include <filesystem>
#include <fstream>
#include "AssetRegistry.h"

using namespace Sailor;

std::string ShaderCache::ShaderCacheEntry::GetCompiledFilepath() const
{
	std::string res;
	std::stringstream stream;
	stream << CompiledShadersFolder << m_UID.ToString() << m_permutation << "." << CompiledShaderFileExtension;
	stream >> res;
	return res;
}

std::string ShaderCache::ShaderCacheEntry::GetPrecompiledFilepath() const
{
	std::string res;
	std::stringstream stream;
	stream << PrecompiledShadersFolder << m_UID.ToString() << m_permutation << "." << PrecompiledShaderFileExtension;
	stream >> res;
	return res;
}

void ShaderCache::ShaderCacheEntry::Serialize(nlohmann::json& outData) const
{
	m_UID.Serialize(outData["uid"]);

	outData["timestamp"] = m_timestamp;
	outData["permutation"] = m_permutation;
}

void ShaderCache::ShaderCacheEntry::Deserialize(const nlohmann::json& inData)
{
	m_UID.Deserialize(inData["uid"]);

	m_timestamp = inData["timestamp"].get<std::time_t>();
	m_permutation = inData["permutation"].get<unsigned int>(); ;
}

void ShaderCache::ShaderCacheData::Serialize(nlohmann::json& outData) const
{
	std::vector<json> data;

	for (const auto& entry : m_data)
	{
		json temp;

		entry.first.Serialize(temp["uid"]);

		std::vector<json> entriesJson;

		for (const auto& permutation : entry.second)
		{
			json temp;
			permutation->Serialize(temp);
			entriesJson.push_back(temp);

		}
		temp["entries"] = entriesJson;

		data.push_back(temp);
	}

	outData["uids"] = data;
}

void ShaderCache::ShaderCacheData::Deserialize(const nlohmann::json& inData)
{
	std::vector<json> data;
	data = inData["uids"].get<std::vector<json>>();

	for (const auto& entryJson : data)
	{
		UID uid;
		uid.Deserialize(entryJson["uid"]);
		std::vector<json> entriesJson = entryJson["entries"].get<std::vector<json>>();

		for (const auto& permutation : entriesJson)
		{
			ShaderCacheEntry* entry = new ShaderCacheEntry();
			entry->Deserialize(permutation);

			m_data[uid].push_back(entry);
		}
	}
}

void ShaderCache::Initialize()
{
	auto shaderCacheFilePath = std::filesystem::path(ShaderCacheFilepath);
	if (!std::filesystem::exists(ShaderCacheFilepath))
	{
		std::ofstream assetFile(ShaderCacheFilepath);

		json dataJson;

		m_cache.Serialize(dataJson);

		assetFile << dataJson.dump();
		assetFile.close();
	}

	std::filesystem::remove_all(PrecompiledShadersFolder);
}

void ShaderCache::Shutdown()
{
	for (const auto& entries : m_cache.m_data)
	{
		for (const auto& entry : entries.second)
		{
			delete entry;
		}
	}
}

void ShaderCache::LoadCache()
{
	std::ifstream assetFile(ShaderCacheFilepath);

	json dataJson;
	assetFile >> dataJson;
	assetFile.close();

	m_cache.Deserialize(dataJson);
}

void ShaderCache::ClearAll()
{
	std::filesystem::remove_all(PrecompiledShadersFolder);
	std::filesystem::remove_all(CompiledShaderFileExtension);
}

void ShaderCache::ClearExpired()
{
	std::vector<UID> clearList;
	for (const auto& entry : m_cache.m_data)
	{
		AssetInfo* assetInfo = AssetRegistry::GetInstance()->GetAssetInfo(entry.first);

		if (bool bIsOutdated = entry.second[0]->m_timestamp < assetInfo->GetAssetLastModificationTime())
		{
			clearList.push_back(entry.first);
		}
	}

	for (const auto& uid : clearList)
	{
		Remove(uid);
	}
}

void ShaderCache::Remove(const UID& uid)
{
	auto it = m_cache.m_data.find(uid);
	if (it != m_cache.m_data.end())
	{
		const auto& entries = *it;

		for (const auto& pEntry : entries.second)
		{
			std::filesystem::remove(pEntry->GetCompiledFilepath());
			std::filesystem::remove(pEntry->GetPrecompiledFilepath());

			delete pEntry;
		}

		m_cache.m_data.erase(it);
	}
}

bool ShaderCache::Contains(UID uid) const
{
	return  m_cache.m_data.find(uid) != m_cache.m_data.end();
}