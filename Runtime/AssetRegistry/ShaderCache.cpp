#include "ShaderCache.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>
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

	LoadCache();
	ClearExpired();
	SaveCache();
}

void ShaderCache::Shutdown()
{
	SaveCache();

	for (const auto& entries : m_cache.m_data)
	{
		for (const auto& entry : entries.second)
		{
			delete entry;
		}
	}
}

void ShaderCache::SaveCache(bool bForcely)
{
	if (bForcely || m_bIsDirty)
	{
		std::ofstream assetFile(ShaderCacheFilepath);

		json cacheJson;
		m_cache.Serialize(cacheJson);

		assetFile << cacheJson.dump();
		assetFile.close();

		m_bIsDirty = false;
	}
}

void ShaderCache::LoadCache()
{
	std::ifstream assetFile(ShaderCacheFilepath);

	json dataJson;
	assetFile >> dataJson;
	assetFile.close();

	m_cache.Deserialize(dataJson);

	m_bIsDirty = false;
}

void ShaderCache::ClearAll()
{
	std::filesystem::remove_all(PrecompiledShadersFolder);
	std::filesystem::remove_all(CompiledShaderFileExtension);
	std::filesystem::remove(ShaderCacheFilepath);
}

void ShaderCache::ClearExpired()
{
	std::vector<UID> expiredShaders;
	std::unordered_set<std::string> whiteListSpirv;

	for (const auto& entries : m_cache.m_data)
	{
		AssetInfo* assetInfo = AssetRegistry::GetInstance()->GetAssetInfo(entries.first);

		if (bool bIsOutdated = entries.second[0]->m_timestamp != assetInfo->GetAssetLastModificationTime())
		{
			expiredShaders.push_back(entries.first);
		}
		else
		{
			for (const auto& entry : entries.second)
			{
				//Convert to the same path format
				auto filepath = std::filesystem::path(entry->GetCompiledFilepath());

				whiteListSpirv.insert(filepath.string());

				if (!std::filesystem::exists(filepath))
				{
					expiredShaders.push_back(entries.first);
				}
			}
		}
	}

	for (const auto& uid : expiredShaders)
	{
		Remove(uid);
	}

	for (const auto& entry : std::filesystem::directory_iterator(CompiledShadersFolder))
	{
		if (entry.is_regular_file() && whiteListSpirv.find(entry.path().string()) == whiteListSpirv.end())
		{
			std::filesystem::remove(entry);
		}
	}

	SaveCache();
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

		m_bIsDirty = true;
		m_cache.m_data.erase(it);
	}
}

bool ShaderCache::Contains(UID uid) const
{
	return m_cache.m_data.find(uid) != m_cache.m_data.end();
}

void ShaderCache::AddSpirv(const UID& uid, unsigned int permutation, const std::string& spirv, const std::string& glsl)
{
	AssetInfo* assetInfo = AssetRegistry::GetInstance()->GetAssetInfo(uid);

	ShaderCacheEntry* newEntry = new ShaderCacheEntry();
	newEntry->m_permutation = permutation;
	newEntry->m_UID = uid;
	newEntry->m_timestamp = assetInfo->GetAssetLastModificationTime();

	if (m_bSavePrecompiledShaders)
	{
		std::ofstream precompiled(newEntry->GetPrecompiledFilepath());
		precompiled << glsl;
		precompiled.close();
	}

	std::ofstream compiled(newEntry->GetCompiledFilepath());
	compiled << spirv;
	compiled.close();

	m_cache.m_data[uid].push_back(newEntry);

	m_bIsDirty = true;
}

