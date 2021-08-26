#include "ShaderCache.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>
#include "AssetRegistry.h"
#include "AssetRegistry/ShaderCompiler.h"


using namespace Sailor;

std::string ShaderCache::GetCachedShaderFilepath(const UID& uid, int32_t permutation, const std::string& shaderKind, bool bIsCompiledToSpirv)
{
	SAILOR_PROFILE_FUNCTION();

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
	m_permutation = inData["permutation"].get<uint32_t>(); ;
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
	SAILOR_PROFILE_FUNCTION();

	std::filesystem::create_directory(CacheRootFolder);
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
	SAILOR_PROFILE_FUNCTION();

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
	SAILOR_PROFILE_FUNCTION();

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
	SAILOR_PROFILE_FUNCTION();

	std::vector<UID> expiredShaders;
	std::unordered_set<std::string> whiteListSpirv;
	std::vector<const ShaderCacheEntry*> blackListEntry;

	for (const auto& entries : m_cache.m_data)
	{
		if (Contains(entries.first))
		{
			for (const auto& entry : entries.second)
			{
				if (!IsExpired(entry->m_UID, entry->m_permutation))
				{
					//Convert to the same path format
					auto vertexFilepath = std::filesystem::path(GetCachedShaderFilepath(entry->m_UID, entry->m_permutation, "VERTEX", true));
					auto fragmentFilepath = std::filesystem::path(GetCachedShaderFilepath(entry->m_UID, entry->m_permutation, "FRAGMENT", true));

					whiteListSpirv.insert(vertexFilepath.string());
					whiteListSpirv.insert(fragmentFilepath.string());
				}
				else
				{
					blackListEntry.push_back(entry);
				}
			}
		}
	}

	for (const auto& entry : blackListEntry)
	{
		Remove(entry);
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

void ShaderCache::Remove(const ShaderCacheEntry* pEntry)
{
	SAILOR_PROFILE_FUNCTION();

	auto it = m_cache.m_data.find(pEntry->m_UID);
	if (it != m_cache.m_data.end())
	{
		UID uid = pEntry->m_UID;

		std::filesystem::remove(GetCachedShaderFilepath(pEntry->m_UID, pEntry->m_permutation, "VERTEX", true));
		std::filesystem::remove(GetCachedShaderFilepath(pEntry->m_UID, pEntry->m_permutation, "FRAGMENT", true));
		std::filesystem::remove(GetCachedShaderFilepath(pEntry->m_UID, pEntry->m_permutation, "VERTEX", false));
		std::filesystem::remove(GetCachedShaderFilepath(pEntry->m_UID, pEntry->m_permutation, "FRAGMENT", false));

		auto& entries = m_cache.m_data[pEntry->m_UID];
		entries.erase(std::find(std::begin(entries), std::end(entries), pEntry));

		delete pEntry;
		m_bIsDirty = true;

		if (entries.size() == 0)
		{
			Remove(uid);
		}
	}
}


void ShaderCache::Remove(const UID& uid)
{
	SAILOR_PROFILE_FUNCTION();

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

void ShaderCache::SavePrecompiledGlsl(const UID& uid, uint32_t permutation, const std::string& vertexGlsl, const std::string& fragmentGlsl) const
{
	SAILOR_PROFILE_FUNCTION();

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

void ShaderCache::CacheSpirv_ThreadSafe(const UID& uid, uint32_t permutation, const std::vector<uint32_t>& vertexSpirv, const std::vector<uint32_t>& fragmentSpirv)
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lk(m_saveToCacheMutex);

	AssetInfo* assetInfo = AssetRegistry::GetInstance()->GetAssetInfo(uid);

	auto it = std::find_if(std::begin(m_cache.m_data[uid]), std::end(m_cache.m_data[uid]),
		[permutation](const ShaderCacheEntry* arg) { return arg->m_permutation == permutation; });

	const bool bAlreadyContains = it != std::end(m_cache.m_data[uid]);

	ShaderCacheEntry* newEntry = bAlreadyContains ? *it : new ShaderCacheEntry();
	newEntry->m_permutation = permutation;
	newEntry->m_UID = uid;
	newEntry->m_timestamp = assetInfo->GetAssetLastModificationTime();

	std::ofstream vertexCompiled(GetCachedShaderFilepath(newEntry->m_UID, newEntry->m_permutation, "VERTEX", true), std::ofstream::binary);
	vertexCompiled.write(reinterpret_cast<const char*>(&vertexSpirv[0]), vertexSpirv.size() * sizeof(uint32_t));
	vertexCompiled.close();

	std::ofstream fragmentCompiled(GetCachedShaderFilepath(newEntry->m_UID, newEntry->m_permutation, "FRAGMENT", true), std::ofstream::binary);
	fragmentCompiled.write(reinterpret_cast<const char*>(&fragmentSpirv[0]), fragmentSpirv.size() * sizeof(uint32_t));
	fragmentCompiled.close();

	if (!bAlreadyContains)
	{
		m_cache.m_data[uid].push_back(newEntry);
	}

	m_bIsDirty = true;
}

bool ShaderCache::GetSpirvCode(const UID& uid, uint32_t permutation, std::vector<uint32_t>& vertexSpirv, std::vector<uint32_t>& fragmentSpirv) const
{
	SAILOR_PROFILE_FUNCTION();

	if (IsExpired(uid, permutation))
	{
		return false;
	}

	const auto& entries = m_cache.m_data.at(uid);
	const auto it = std::find_if(std::cbegin(entries), std::cend(entries),
		[permutation](const ShaderCacheEntry* arg) { return arg->m_permutation == permutation; });

	std::string vertexFilepath = GetCachedShaderFilepath((*it)->m_UID, (*it)->m_permutation, "VERTEX", true);
	std::string fragmentFilepath = GetCachedShaderFilepath((*it)->m_UID, (*it)->m_permutation, "FRAGMENT", true);

	AssetRegistry::ReadBinaryFile(vertexFilepath, vertexSpirv);
	AssetRegistry::ReadBinaryFile(fragmentFilepath, fragmentSpirv);

	return true;
}

bool ShaderCache::IsExpired(const UID& uid, uint32_t permutation) const
{
	if (!Contains(uid))
	{
		return true;
	}

	const auto& entries = m_cache.m_data.at(uid);
	const auto it = std::find_if(std::cbegin(entries), std::cend(entries),
		[permutation](const ShaderCacheEntry* arg) { return arg->m_permutation == permutation; });

	const bool bAlreadyContains = it != std::cend(entries);

	if (!bAlreadyContains)
	{
		return true;
	}

	auto vertexFilepath = std::filesystem::path(GetCachedShaderFilepath(uid, permutation, "VERTEX", true));
	auto fragmentFilepath = std::filesystem::path(GetCachedShaderFilepath(uid, permutation, "FRAGMENT", true));

	if (!(std::filesystem::exists(vertexFilepath) && std::filesystem::exists(fragmentFilepath)))
	{
		return true;
	}

	AssetInfo* assetInfo = AssetRegistry::GetInstance()->GetAssetInfo(uid);

	return (*it)->m_timestamp < assetInfo->GetAssetLastModificationTime();
}


