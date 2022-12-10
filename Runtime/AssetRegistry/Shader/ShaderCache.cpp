#include "ShaderCache.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include "Containers/Set.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"

using namespace Sailor;

std::filesystem::path ShaderCache::GetPrecompiledShaderFilepath(const UID& uid, int32_t permutation, const std::string& shaderKind)
{
	std::string res;
	std::stringstream stream;
	stream << uid.ToString() << shaderKind << permutation << "." << PrecompiledShaderFileExtension;
	stream >> res;
	return res;
}

std::filesystem::path ShaderCache::GetCachedShaderFilepath(const UID& uid, int32_t permutation, const std::string& shaderKind)
{
	std::string res;
	std::stringstream stream;
	stream << CompiledShadersFolder << uid.ToString() << shaderKind << permutation << "." << CompiledShaderFileExtension;
	stream >> res;
	return res;
}

std::filesystem::path ShaderCache::GetCachedShaderWithDebugFilepath(const UID& uid, int32_t permutation, const std::string& shaderKind)
{
	std::string res;
	std::stringstream stream;
	stream << CompiledShadersWithDebugFolder << uid.ToString() << shaderKind << permutation << "." << CompiledShaderFileExtension;
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
	TVector<json> data;

	for (const auto& entry : m_data)
	{
		json temp;

		entry.m_first.Serialize(temp["uid"]);

		std::vector<json> entriesJson;

		for (const auto& permutation : entry.m_second)
		{
			json temp;
			permutation->Serialize(temp);
			entriesJson.push_back(temp);

		}
		temp["entries"] = entriesJson;

		data.Add(temp);
	}

	outData["uids"] = data;
}

void ShaderCache::ShaderCacheData::Deserialize(const nlohmann::json& inData)
{
	TVector<json> data;
	data = inData["uids"].get<TVector<json>>();

	for (const auto& entryJson : data)
	{
		UID uid;
		uid.Deserialize(entryJson["uid"]);
		TVector<json> entriesJson = entryJson["entries"].get<TVector<json>>();

		for (const auto& permutation : entriesJson)
		{
			ShaderCacheEntry* entry = new ShaderCacheEntry();
			entry->Deserialize(permutation);

			m_data[uid].Add(entry);
		}
	}
}

void ShaderCache::Initialize()
{
	SAILOR_PROFILE_FUNCTION();

	std::filesystem::create_directory(CacheRootFolder);
	std::filesystem::create_directory(CompiledShadersFolder);
	std::filesystem::create_directory(PrecompiledShadersFolder);
	std::filesystem::create_directory(CompiledShadersWithDebugFolder);

	auto shaderCacheFilePath = std::filesystem::path(ShaderCacheFilepath);
	if (!std::filesystem::exists(ShaderCacheFilepath))
	{
		std::ofstream assetFile(ShaderCacheFilepath);

		json dataJson;

		m_cache.Serialize(dataJson);

		assetFile << dataJson.dump(Sailor::JsonDumpIndent);
		assetFile.close();
	}

	LoadCache();
}

void ShaderCache::Shutdown()
{
	ClearExpired();
	SaveCache();

	for (const auto& entries : m_cache.m_data)
	{
		for (const auto& entry : entries.m_second)
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

		assetFile << cacheJson.dump(Sailor::JsonDumpIndent);
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
	std::lock_guard<std::mutex> lk(m_saveToCacheMutex);

	std::filesystem::remove_all(PrecompiledShadersFolder);
	std::filesystem::remove_all(CompiledShaderFileExtension);
	std::filesystem::remove_all(CompiledShadersWithDebugFolder);
	std::filesystem::remove(ShaderCacheFilepath);
}

void ShaderCache::ClearExpired()
{
	SAILOR_PROFILE_FUNCTION();

	TVector<UID> expiredShaders;
	TSet<std::string> whiteListSpirv;
	TVector<ShaderCacheEntry*> blackListEntry;

	for (const auto& entries : m_cache.m_data)
	{
		if (Contains(entries.m_first))
		{
			for (const auto& entry : entries.m_second)
			{
				if (!IsExpired(entry->m_UID, entry->m_permutation))
				{
					//Convert to the same path format
					auto vertexFilepath = GetCachedShaderFilepath(entry->m_UID, entry->m_permutation, ShaderCache::VertexShaderTag);
					auto fragmentFilepath = GetCachedShaderFilepath(entry->m_UID, entry->m_permutation, ShaderCache::FragmentShaderTag);
					auto computeFilepath = GetCachedShaderFilepath(entry->m_UID, entry->m_permutation, ShaderCache::ComputeShaderTag);

					whiteListSpirv.Insert(vertexFilepath.string());
					whiteListSpirv.Insert(fragmentFilepath.string());
					whiteListSpirv.Insert(computeFilepath.string());
				}
				else
				{
					blackListEntry.Add(entry);
				}
			}
		}
	}

	for (auto& entry : blackListEntry)
	{
		Remove(entry);
	}

	for (const auto& entry : std::filesystem::directory_iterator(CompiledShadersFolder))
	{
		if (entry.is_regular_file() && !whiteListSpirv.Contains(entry.path().string()))
		{
			std::filesystem::remove(entry);
		}
	}

	SaveCache();
}

void ShaderCache::Remove(ShaderCacheEntry* pEntry)
{
	SAILOR_PROFILE_FUNCTION();

	auto it = m_cache.m_data.Find(pEntry->m_UID);
	if (it != m_cache.m_data.end())
	{
		UID uid = pEntry->m_UID;

		std::filesystem::remove(GetCachedShaderFilepath(pEntry->m_UID, pEntry->m_permutation, ShaderCache::VertexShaderTag));
		std::filesystem::remove(GetCachedShaderFilepath(pEntry->m_UID, pEntry->m_permutation, ShaderCache::FragmentShaderTag));
		std::filesystem::remove(GetCachedShaderFilepath(pEntry->m_UID, pEntry->m_permutation, ShaderCache::ComputeShaderTag));

		std::filesystem::remove(GetCachedShaderWithDebugFilepath(pEntry->m_UID, pEntry->m_permutation, ShaderCache::VertexShaderTag));
		std::filesystem::remove(GetCachedShaderWithDebugFilepath(pEntry->m_UID, pEntry->m_permutation, ShaderCache::FragmentShaderTag));
		std::filesystem::remove(GetCachedShaderWithDebugFilepath(pEntry->m_UID, pEntry->m_permutation, ShaderCache::ComputeShaderTag));

		std::filesystem::remove(GetPrecompiledShaderFilepath(pEntry->m_UID, pEntry->m_permutation, ShaderCache::VertexShaderTag));
		std::filesystem::remove(GetPrecompiledShaderFilepath(pEntry->m_UID, pEntry->m_permutation, ShaderCache::FragmentShaderTag));
		std::filesystem::remove(GetPrecompiledShaderFilepath(pEntry->m_UID, pEntry->m_permutation, ShaderCache::ComputeShaderTag));

		auto& entries = m_cache.m_data[pEntry->m_UID];

		entries.Remove(pEntry);

		delete pEntry;
		m_bIsDirty = true;

		if (entries.Num() == 0)
		{
			Remove(uid);
		}
	}
}

void ShaderCache::Remove(const UID& uid)
{
	SAILOR_PROFILE_FUNCTION();
	
	std::lock_guard<std::mutex> lk(m_saveToCacheMutex);

	auto it = m_cache.m_data.Find(uid);
	if (it != m_cache.m_data.end())
	{
		const auto& entries = *it;

		for (const auto& pEntry : entries.m_second)
		{
			std::filesystem::remove(GetCachedShaderFilepath(pEntry->m_UID, pEntry->m_permutation, ShaderCache::VertexShaderTag));
			std::filesystem::remove(GetCachedShaderFilepath(pEntry->m_UID, pEntry->m_permutation, ShaderCache::FragmentShaderTag));
			std::filesystem::remove(GetCachedShaderFilepath(pEntry->m_UID, pEntry->m_permutation, ShaderCache::ComputeShaderTag));

			std::filesystem::remove(GetCachedShaderWithDebugFilepath(pEntry->m_UID, pEntry->m_permutation, ShaderCache::VertexShaderTag));
			std::filesystem::remove(GetCachedShaderWithDebugFilepath(pEntry->m_UID, pEntry->m_permutation, ShaderCache::FragmentShaderTag));
			std::filesystem::remove(GetCachedShaderWithDebugFilepath(pEntry->m_UID, pEntry->m_permutation, ShaderCache::ComputeShaderTag));

			std::filesystem::remove(GetPrecompiledShaderFilepath(pEntry->m_UID, pEntry->m_permutation, ShaderCache::VertexShaderTag));
			std::filesystem::remove(GetPrecompiledShaderFilepath(pEntry->m_UID, pEntry->m_permutation, ShaderCache::FragmentShaderTag));
			std::filesystem::remove(GetPrecompiledShaderFilepath(pEntry->m_UID, pEntry->m_permutation, ShaderCache::ComputeShaderTag));

			delete pEntry;
		}

		m_bIsDirty = true;
		m_cache.m_data.Remove(it->m_first);
	}
}

bool ShaderCache::Contains(const UID& uid) const
{
	return m_cache.m_data.ContainsKey(uid);
}

void ShaderCache::CachePrecompiledGlsl(const UID& uid, uint32_t permutation, const std::string& vertexGlsl, const std::string& fragmentGlsl, const std::string& computeGlsl)
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lk(m_saveToCacheMutex);

	if (m_bSavePrecompiledGlsl)
	{
		if (!vertexGlsl.empty())
		{
			std::ofstream vertexPrecompiled(GetPrecompiledShaderFilepath(uid, permutation, ShaderCache::VertexShaderTag));
			vertexPrecompiled << vertexGlsl;
			vertexPrecompiled.close();
		}

		if (!fragmentGlsl.empty())
		{
			std::ofstream fragmentPrecompiled(GetPrecompiledShaderFilepath(uid, permutation, ShaderCache::FragmentShaderTag));
			fragmentPrecompiled << fragmentGlsl;
			fragmentPrecompiled.close();
		}

		if (!computeGlsl.empty())
		{
			std::ofstream computePrecompiled(GetPrecompiledShaderFilepath(uid, permutation, ShaderCache::ComputeShaderTag));
			computePrecompiled << computeGlsl;
			computePrecompiled.close();
		}
	}
}

void ShaderCache::CacheSpirvWithDebugInfo(const UID& uid, uint32_t permutation, const TVector<uint32_t>& vertexSpirv, const TVector<uint32_t>& fragmentSpirv, const TVector<uint32_t>& computeSpirv)
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lk(m_saveToCacheMutex);

	AssetInfoPtr assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(uid);

	if (vertexSpirv.Num() > 0)
	{
		std::ofstream vertexCompiled(GetCachedShaderWithDebugFilepath(uid, permutation, ShaderCache::VertexShaderTag), std::ofstream::binary);
		vertexCompiled.write(reinterpret_cast<const char*>(&vertexSpirv[0]), vertexSpirv.Num() * sizeof(uint32_t));
		vertexCompiled.close();
	}

	if (fragmentSpirv.Num() > 0)
	{
		std::ofstream fragmentCompiled(GetCachedShaderWithDebugFilepath(uid, permutation, ShaderCache::FragmentShaderTag), std::ofstream::binary);
		fragmentCompiled.write(reinterpret_cast<const char*>(&fragmentSpirv[0]), fragmentSpirv.Num() * sizeof(uint32_t));
		fragmentCompiled.close();
	}

	if (computeSpirv.Num() > 0)
	{
		std::ofstream computeCompiled(GetCachedShaderWithDebugFilepath(uid, permutation, ShaderCache::ComputeShaderTag), std::ofstream::binary);
		computeCompiled.write(reinterpret_cast<const char*>(&computeSpirv[0]), computeSpirv.Num() * sizeof(uint32_t));
		computeCompiled.close();
	}
}

void ShaderCache::CacheSpirv_ThreadSafe(const UID& uid, uint32_t permutation, const TVector<uint32_t>& vertexSpirv, const TVector<uint32_t>& fragmentSpirv, const TVector<uint32_t>& computeSpirv)
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lk(m_saveToCacheMutex);

	AssetInfoPtr assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(uid);

	auto it = std::find_if(std::begin(m_cache.m_data[uid]), std::end(m_cache.m_data[uid]),
		[permutation](const ShaderCacheEntry* arg) { return arg->m_permutation == permutation; });

	const bool bAlreadyContains = it != std::end(m_cache.m_data[uid]);

	time_t timeStamp = 0;
	const bool hasAsset = GetTimeStamp(uid, timeStamp);

	ShaderCacheEntry* newEntry = bAlreadyContains ? *it : new ShaderCacheEntry();
	newEntry->m_permutation = permutation;
	newEntry->m_UID = uid;
	newEntry->m_timestamp = timeStamp;

	if (vertexSpirv.Num() > 0)
	{
		std::ofstream vertexCompiled(GetCachedShaderFilepath(newEntry->m_UID, newEntry->m_permutation, ShaderCache::VertexShaderTag), std::ofstream::binary);
		vertexCompiled.write(reinterpret_cast<const char*>(&vertexSpirv[0]), vertexSpirv.Num() * sizeof(uint32_t));
		vertexCompiled.close();
	}

	if (fragmentSpirv.Num() > 0)
	{
		std::ofstream fragmentCompiled(GetCachedShaderFilepath(newEntry->m_UID, newEntry->m_permutation, ShaderCache::FragmentShaderTag), std::ofstream::binary);
		fragmentCompiled.write(reinterpret_cast<const char*>(&fragmentSpirv[0]), fragmentSpirv.Num() * sizeof(uint32_t));
		fragmentCompiled.close();
	}

	if (computeSpirv.Num() > 0)
	{
		std::ofstream computeCompiled(GetCachedShaderFilepath(newEntry->m_UID, newEntry->m_permutation, ShaderCache::ComputeShaderTag), std::ofstream::binary);
		computeCompiled.write(reinterpret_cast<const char*>(&computeSpirv[0]), computeSpirv.Num() * sizeof(uint32_t));
		computeCompiled.close();
	}

	if (!bAlreadyContains)
	{
		m_cache.m_data[uid].Add(newEntry);
	}

	m_bIsDirty = true;
}

bool ShaderCache::GetSpirvCode(const UID& uid, uint32_t permutation, TVector<uint32_t>& vertexSpirv, TVector<uint32_t>& fragmentSpirv, TVector<uint32_t>& computeSpirv, bool bIsDebug)
{
	SAILOR_PROFILE_FUNCTION();
	
	std::lock_guard<std::mutex> lk(m_saveToCacheMutex);

	if (IsExpired(uid, permutation))
	{
		return false;
	}

	const auto& entries = m_cache.m_data[uid];
	const auto it = std::find_if(std::cbegin(entries), std::cend(entries),
		[permutation](const ShaderCacheEntry* arg) { return arg->m_permutation == permutation; });

	std::filesystem::path vertexFilepath = bIsDebug ?
		GetCachedShaderWithDebugFilepath((*it)->m_UID, (*it)->m_permutation, ShaderCache::VertexShaderTag) :
		GetCachedShaderFilepath((*it)->m_UID, (*it)->m_permutation, ShaderCache::VertexShaderTag);

	std::filesystem::path fragmentFilepath = bIsDebug ?
		GetCachedShaderWithDebugFilepath((*it)->m_UID, (*it)->m_permutation, ShaderCache::FragmentShaderTag) :
		GetCachedShaderFilepath((*it)->m_UID, (*it)->m_permutation, ShaderCache::FragmentShaderTag);

	std::filesystem::path computeFilepath = bIsDebug ?
		GetCachedShaderWithDebugFilepath((*it)->m_UID, (*it)->m_permutation, ShaderCache::ComputeShaderTag) :
		GetCachedShaderFilepath((*it)->m_UID, (*it)->m_permutation, ShaderCache::ComputeShaderTag);

	AssetRegistry::ReadBinaryFile(vertexFilepath, vertexSpirv);
	AssetRegistry::ReadBinaryFile(fragmentFilepath, fragmentSpirv);
	AssetRegistry::ReadBinaryFile(computeFilepath, computeSpirv);

	return true;
}

bool ShaderCache::IsExpired(const UID& uid, uint32_t permutation) const
{
	if (!Contains(uid))
	{
		return true;
	}

	const auto& entries = m_cache.m_data[uid];
	size_t index = entries.FindIf([permutation](const ShaderCacheEntry* arg) { return arg->m_permutation == permutation; });

	const bool bAlreadyContains = index != -1;

	if (!bAlreadyContains)
	{
		return true;
	}

	auto vertexFilepath = std::filesystem::path(GetCachedShaderFilepath(uid, permutation, ShaderCache::VertexShaderTag));
	auto fragmentFilepath = std::filesystem::path(GetCachedShaderFilepath(uid, permutation, ShaderCache::FragmentShaderTag));
	auto computeFilepath = std::filesystem::path(GetCachedShaderFilepath(uid, permutation, ShaderCache::ComputeShaderTag));

	if (!(std::filesystem::exists(vertexFilepath) && std::filesystem::exists(fragmentFilepath)) && !std::filesystem::exists(computeFilepath))
	{
		return true;
	}

	time_t timeStamp = 0;
	const bool hasAsset = GetTimeStamp(uid, timeStamp);

	return hasAsset ? entries[index]->m_timestamp < timeStamp : true;
}

bool ShaderCache::GetTimeStamp(const UID& uid, time_t& outTimestamp) const
{
	if (ShaderAssetInfoPtr assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<ShaderAssetInfoPtr>(uid))
	{
		if (TSharedPtr<ShaderAsset> shaderAsset = App::GetSubmodule<ShaderCompiler>()->LoadShaderAsset(assetInfo->GetUID()).Lock())
		{
			outTimestamp = assetInfo->GetAssetLastModificationTime();
			for (const auto& include : shaderAsset->GetIncludes())
			{
				outTimestamp = std::max(outTimestamp, Sailor::Utils::GetFileModificationTime(AssetRegistry::ContentRootFolder + include));
			}
			return true;
		}
	}
	return false;
}