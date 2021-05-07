#include "ShaderCache.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>
#include "AssetRegistry.h"

using namespace Sailor;

std::string ShaderCache::GetCachedShaderFilepath(const UID& uid, int permutation, const std::string& shaderKind, bool bIsCompiledToSpirv)
{
	std::string res;
	std::stringstream stream;
	stream << (bIsCompiledToSpirv ? CompiledShadersFolder : PrecompiledShadersFolder)
		<< uid.ToString()
		<< shaderKind
		<< permutation
		<< "."
		<< (bIsCompiledToSpirv ? CompiledShaderFileExtension : PrecompiledShaderFileExtension);

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
	std::filesystem::create_directory(CompiledShadersFolder);
	std::filesystem::create_directory(PrecompiledShadersFolder);
	
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
		if (IsExpired(entries.first))
		{
			expiredShaders.push_back(entries.first);
		}
		else
		{
			for (const auto& entry : entries.second)
			{
				//Convert to the same path format
				auto vertexFilepath = std::filesystem::path(GetCachedShaderFilepath(entry->m_UID, entry->m_permutation, "VERTEX", true));
				auto fragmentFilepath = std::filesystem::path(GetCachedShaderFilepath(entry->m_UID, entry->m_permutation, "FRAGMENT", true));

				whiteListSpirv.insert(vertexFilepath.string());
				whiteListSpirv.insert(fragmentFilepath.string());
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
			std::filesystem::remove(GetCachedShaderFilepath(pEntry->m_UID, pEntry->m_permutation, "VERTEX", true));
			std::filesystem::remove(GetCachedShaderFilepath(pEntry->m_UID, pEntry->m_permutation, "FRAGMENT", true));
			std::filesystem::remove(GetCachedShaderFilepath(pEntry->m_UID, pEntry->m_permutation, "VERTEX", false));
			std::filesystem::remove(GetCachedShaderFilepath(pEntry->m_UID, pEntry->m_permutation, "FRAGMENT", false));

			delete pEntry;
		}

		m_bIsDirty = true;
		m_cache.m_data.erase(it);
	}
}

bool ShaderCache::Contains(const UID& uid) const
{
	return m_cache.m_data.find(uid) != m_cache.m_data.end();
}

bool ShaderCache::IsExpired(const UID& uid) const
{
	if (!Contains(uid))
	{
		return true;
	}

	const auto& entries = m_cache.m_data.at(uid);
	AssetInfo* assetInfo = AssetRegistry::GetInstance()->GetAssetInfo(uid);

	if (bool bIsOutdated = entries[0]->m_timestamp != assetInfo->GetAssetLastModificationTime())
	{
		return true;
	}

	for (const auto& entry : entries)
	{
		//Convert to the same path format
		auto vertexFilepath = std::filesystem::path(GetCachedShaderFilepath(entry->m_UID, entry->m_permutation, "VERTEX", true));
		auto fragmentFilepath = std::filesystem::path(GetCachedShaderFilepath(entry->m_UID, entry->m_permutation, "FRAGMENT", true));

		if (!(std::filesystem::exists(vertexFilepath) && std::filesystem::exists(fragmentFilepath)))
		{
			return true;
		}
	}

	return false;
}

void ShaderCache::SavePrecompiledGlsl(const UID& uid, unsigned int permutation, const std::string& vertexGlsl, const std::string& fragmentGlsl) const
{
	if (m_bSavePrecompiledGlsl)
	{
		std::ofstream vertexPrecompiled(GetCachedShaderFilepath(uid, permutation, "VERTEX", false));
		vertexPrecompiled << vertexGlsl;
		vertexPrecompiled.close();

		std::ofstream fragmentPrecompiled(GetCachedShaderFilepath(uid, permutation, "FRAGMENT", false));
		fragmentPrecompiled << fragmentGlsl;
		fragmentPrecompiled.close();
	}
}

void ShaderCache::CacheSpirv(const UID& uid, unsigned int permutation, const std::vector<uint32_t>& vertexSpirv, const std::vector<uint32_t>& fragmentSpirv)
{
	AssetInfo* assetInfo = AssetRegistry::GetInstance()->GetAssetInfo(uid);

	ShaderCacheEntry* newEntry = new ShaderCacheEntry();
	newEntry->m_permutation = permutation;
	newEntry->m_UID = uid;
	newEntry->m_timestamp = assetInfo->GetAssetLastModificationTime();

	std::ofstream vertexCompiled(GetCachedShaderFilepath(newEntry->m_UID, newEntry->m_permutation, "VERTEX", true), std::ofstream::binary);
	vertexCompiled.write(reinterpret_cast<const char*>(&vertexSpirv[0]), vertexSpirv.size() * sizeof(uint32_t));
	vertexCompiled.close();

	std::ofstream fragmentCompiled(GetCachedShaderFilepath(newEntry->m_UID, newEntry->m_permutation, "FRAGMENT", true), std::ofstream::binary);
	fragmentCompiled.write(reinterpret_cast<const char*>(&fragmentSpirv[0]), fragmentSpirv.size() * sizeof(uint32_t));
	fragmentCompiled.close();

	m_cache.m_data[uid].push_back(newEntry);

	m_bIsDirty = true;
}

