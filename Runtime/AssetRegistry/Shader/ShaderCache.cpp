#include "ShaderCache.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include "Containers/Set.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"

using namespace Sailor;

std::filesystem::path ShaderCache::GetPrecompiledShaderFilepath(const FileId& uid, int32_t permutation, const std::string& shaderKind)
{
	std::string res;
	std::stringstream stream;
	stream << uid.ToString() << shaderKind << permutation << "." << PrecompiledShaderFileExtension;
	stream >> res;
	return res;
}

std::filesystem::path ShaderCache::GetCachedShaderFilepath(const FileId& uid, int32_t permutation, const std::string& shaderKind)
{
	std::string res;
	std::stringstream stream;
	stream << GetCompiledShadersFolder() << uid.ToString() << shaderKind << permutation << "." << CompiledShaderFileExtension;
	stream >> res;
	return res;
}

std::filesystem::path ShaderCache::GetCachedShaderWithDebugFilepath(const FileId& uid, int32_t permutation, const std::string& shaderKind)
{
	std::string res;
	std::stringstream stream;
	stream << GetCompiledShadersWithDebugFolder() << uid.ToString() << shaderKind << permutation << "." << CompiledShaderFileExtension;
	stream >> res;
	return res;
}

YAML::Node ShaderCache::ShaderCacheData::Entry::Serialize() const
{
	YAML::Node res;

	res["fileId"] = m_fileId;
	res["timestamp"] = m_timestamp;
	res["permutation"] = m_permutation;

	return res;
}

void ShaderCache::ShaderCacheData::Entry::Deserialize(const YAML::Node& inData)
{
	m_fileId = inData["fileId"].as<FileId>();
	m_timestamp = inData["timestamp"].as<std::time_t>();
	m_permutation = inData["permutation"].as<uint32_t>();
}

YAML::Node ShaderCache::ShaderCacheData::Serialize() const
{
	YAML::Node res;
	::Serialize(res, "entries", m_data);

	return res;
}

void ShaderCache::ShaderCacheData::Deserialize(const YAML::Node& inData)
{
	::Deserialize(inData, "entries", m_data);
}

void ShaderCache::Initialize()
{
	SAILOR_PROFILE_FUNCTION();

	std::filesystem::create_directory(AssetRegistry::GetCacheFolder());
	std::filesystem::create_directory(GetCompiledShadersFolder());
	std::filesystem::create_directory(GetPrecompiledShadersFolder());
	std::filesystem::create_directory(GetCompiledShadersWithDebugFolder());

	auto shaderCacheFilePath = std::filesystem::path(GetShaderCacheFilepath());
	if (!std::filesystem::exists(GetShaderCacheFilepath()))
	{
		YAML::Node outData;
		outData["shaderCache"] = m_cache.Serialize();

		std::ofstream assetFile(GetShaderCacheFilepath());
		assetFile << outData;
		assetFile.close();
	}

	LoadCache();
}

void ShaderCache::Shutdown()
{
	ClearExpired();
	SaveCache();
}

void ShaderCache::SaveCache(bool bForcely)
{
	SAILOR_PROFILE_FUNCTION();

	if (bForcely || m_bIsDirty)
	{
		YAML::Node outData;
		outData["shaderCache"] = m_cache.Serialize();

		std::ofstream assetFile(GetShaderCacheFilepath());
		assetFile << outData;
		assetFile.close();

		m_bIsDirty = false;
	}
}

void ShaderCache::LoadCache()
{
	SAILOR_PROFILE_FUNCTION();

	std::string yaml;
	AssetRegistry::ReadAllTextFile(GetShaderCacheFilepath(), yaml);
	YAML::Node yamlNode = YAML::Load(yaml);
	m_cache.Deserialize(yamlNode["shaderCache"]);

	m_bIsDirty = false;
}

void ShaderCache::ClearAll()
{
	std::lock_guard<std::mutex> lk(m_saveToCacheMutex);

	std::filesystem::remove_all(GetPrecompiledShadersFolder());
	std::filesystem::remove_all(CompiledShaderFileExtension);
	std::filesystem::remove_all(GetCompiledShadersWithDebugFolder());
	std::filesystem::remove(GetShaderCacheFilepath());
}

void ShaderCache::ClearExpired()
{
	SAILOR_PROFILE_FUNCTION();

	TVector<FileId> expiredShaders;
	TSet<std::string> whiteListSpirv;
	TVector<ShaderCacheData::Entry> blackListEntry;

	for (const auto& entries : m_cache.m_data)
	{
		if (Contains(entries.m_first))
		{
			for (const auto& entry : *entries.m_second)
			{
				if (!IsExpired(entry.m_fileId, entry.m_permutation))
				{
					//Convert to the same path format
					auto vertexFilepath = GetCachedShaderFilepath(entry.m_fileId, entry.m_permutation, ShaderCache::VertexShaderTag);
					auto fragmentFilepath = GetCachedShaderFilepath(entry.m_fileId, entry.m_permutation, ShaderCache::FragmentShaderTag);
					auto computeFilepath = GetCachedShaderFilepath(entry.m_fileId, entry.m_permutation, ShaderCache::ComputeShaderTag);

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

	for (const auto& entry : std::filesystem::directory_iterator(GetCompiledShadersFolder()))
	{
		if (entry.is_regular_file() && !whiteListSpirv.Contains(entry.path().string()))
		{
			std::filesystem::remove(entry);
		}
	}

	SaveCache();
}

void ShaderCache::Remove(const ShaderCacheData::Entry& pEntry)
{
	SAILOR_PROFILE_FUNCTION();

	auto it = m_cache.m_data.Find(pEntry.m_fileId);
	if (it != m_cache.m_data.end())
	{
		FileId uid = pEntry.m_fileId;

		std::filesystem::remove(GetCachedShaderFilepath(pEntry.m_fileId, pEntry.m_permutation, ShaderCache::VertexShaderTag));
		std::filesystem::remove(GetCachedShaderFilepath(pEntry.m_fileId, pEntry.m_permutation, ShaderCache::FragmentShaderTag));
		std::filesystem::remove(GetCachedShaderFilepath(pEntry.m_fileId, pEntry.m_permutation, ShaderCache::ComputeShaderTag));

		std::filesystem::remove(GetCachedShaderWithDebugFilepath(pEntry.m_fileId, pEntry.m_permutation, ShaderCache::VertexShaderTag));
		std::filesystem::remove(GetCachedShaderWithDebugFilepath(pEntry.m_fileId, pEntry.m_permutation, ShaderCache::FragmentShaderTag));
		std::filesystem::remove(GetCachedShaderWithDebugFilepath(pEntry.m_fileId, pEntry.m_permutation, ShaderCache::ComputeShaderTag));

		std::filesystem::remove(GetPrecompiledShaderFilepath(pEntry.m_fileId, pEntry.m_permutation, ShaderCache::VertexShaderTag));
		std::filesystem::remove(GetPrecompiledShaderFilepath(pEntry.m_fileId, pEntry.m_permutation, ShaderCache::FragmentShaderTag));
		std::filesystem::remove(GetPrecompiledShaderFilepath(pEntry.m_fileId, pEntry.m_permutation, ShaderCache::ComputeShaderTag));

		auto& entries = m_cache.m_data[pEntry.m_fileId];

		entries.Remove(pEntry);

		m_bIsDirty = true;

		if (entries.Num() == 0)
		{
			Remove(uid);
		}
	}
}

void ShaderCache::Remove(const FileId& uid)
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lk(m_saveToCacheMutex);

	auto it = m_cache.m_data.Find(uid);
	if (it != m_cache.m_data.end())
	{
		const auto& entries = *it;

		for (const auto& pEntry : *entries.m_second)
		{
			std::filesystem::remove(GetCachedShaderFilepath(pEntry.m_fileId, pEntry.m_permutation, ShaderCache::VertexShaderTag));
			std::filesystem::remove(GetCachedShaderFilepath(pEntry.m_fileId, pEntry.m_permutation, ShaderCache::FragmentShaderTag));
			std::filesystem::remove(GetCachedShaderFilepath(pEntry.m_fileId, pEntry.m_permutation, ShaderCache::ComputeShaderTag));

			std::filesystem::remove(GetCachedShaderWithDebugFilepath(pEntry.m_fileId, pEntry.m_permutation, ShaderCache::VertexShaderTag));
			std::filesystem::remove(GetCachedShaderWithDebugFilepath(pEntry.m_fileId, pEntry.m_permutation, ShaderCache::FragmentShaderTag));
			std::filesystem::remove(GetCachedShaderWithDebugFilepath(pEntry.m_fileId, pEntry.m_permutation, ShaderCache::ComputeShaderTag));

			std::filesystem::remove(GetPrecompiledShaderFilepath(pEntry.m_fileId, pEntry.m_permutation, ShaderCache::VertexShaderTag));
			std::filesystem::remove(GetPrecompiledShaderFilepath(pEntry.m_fileId, pEntry.m_permutation, ShaderCache::FragmentShaderTag));
			std::filesystem::remove(GetPrecompiledShaderFilepath(pEntry.m_fileId, pEntry.m_permutation, ShaderCache::ComputeShaderTag));
		}

		m_bIsDirty = true;
		m_cache.m_data.Remove(it.Key());
	}
}

bool ShaderCache::Contains(const FileId& uid) const
{
	return m_cache.m_data.ContainsKey(uid);
}

void ShaderCache::CachePrecompiledGlsl(const FileId& uid, uint32_t permutation, const std::string& vertexGlsl, const std::string& fragmentGlsl, const std::string& computeGlsl)
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

void ShaderCache::CacheSpirvWithDebugInfo(const FileId& uid, uint32_t permutation, const TVector<uint32_t>& vertexSpirv, const TVector<uint32_t>& fragmentSpirv, const TVector<uint32_t>& computeSpirv)
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lk(m_saveToCacheMutex);

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

void ShaderCache::CacheSpirv_ThreadSafe(const FileId& uid, uint32_t permutation, const TVector<uint32_t>& vertexSpirv, const TVector<uint32_t>& fragmentSpirv, const TVector<uint32_t>& computeSpirv)
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lk(m_saveToCacheMutex);

	auto it = std::find_if(std::begin(m_cache.m_data[uid]), std::end(m_cache.m_data[uid]),
		[permutation](const ShaderCacheData::Entry& arg) { return arg.m_permutation == permutation; });

	const bool bAlreadyContains = it != std::end(m_cache.m_data[uid]);

	time_t timeStamp = 0;
	
	ShaderCacheData::Entry newEntry = ShaderCacheData::Entry();
	ShaderCacheData::Entry* entry = &newEntry;

	if (bAlreadyContains)
	{
		entry = &(*it);
	}

	entry->m_permutation = permutation;
	entry->m_fileId = uid;
	entry->m_timestamp = timeStamp;

	if (vertexSpirv.Num() > 0)
	{
		std::ofstream vertexCompiled(GetCachedShaderFilepath(entry->m_fileId, entry->m_permutation, ShaderCache::VertexShaderTag), std::ofstream::binary);
		vertexCompiled.write(reinterpret_cast<const char*>(&vertexSpirv[0]), vertexSpirv.Num() * sizeof(uint32_t));
		vertexCompiled.close();
	}

	if (fragmentSpirv.Num() > 0)
	{
		std::ofstream fragmentCompiled(GetCachedShaderFilepath(entry->m_fileId, entry->m_permutation, ShaderCache::FragmentShaderTag), std::ofstream::binary);
		fragmentCompiled.write(reinterpret_cast<const char*>(&fragmentSpirv[0]), fragmentSpirv.Num() * sizeof(uint32_t));
		fragmentCompiled.close();
	}

	if (computeSpirv.Num() > 0)
	{
		std::ofstream computeCompiled(GetCachedShaderFilepath(entry->m_fileId, entry->m_permutation, ShaderCache::ComputeShaderTag), std::ofstream::binary);
		computeCompiled.write(reinterpret_cast<const char*>(&computeSpirv[0]), computeSpirv.Num() * sizeof(uint32_t));
		computeCompiled.close();
	}

	if (!bAlreadyContains)
	{
		m_cache.m_data[uid].Add(*entry);
	}

	m_bIsDirty = true;
}

bool ShaderCache::GetSpirvCode(const FileId& uid, uint32_t permutation, TVector<uint32_t>& vertexSpirv, TVector<uint32_t>& fragmentSpirv, TVector<uint32_t>& computeSpirv, bool bIsDebug)
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lk(m_saveToCacheMutex);

	if (IsExpired(uid, permutation))
	{
		return false;
	}

	const auto& entries = m_cache.m_data[uid];
	const auto it = std::find_if(std::cbegin(entries), std::cend(entries),
		[permutation](const ShaderCacheData::Entry& arg) { return arg.m_permutation == permutation; });

	std::filesystem::path vertexFilepath = bIsDebug ?
		GetCachedShaderWithDebugFilepath((*it).m_fileId, (*it).m_permutation, ShaderCache::VertexShaderTag) :
		GetCachedShaderFilepath((*it).m_fileId, (*it).m_permutation, ShaderCache::VertexShaderTag);

	std::filesystem::path fragmentFilepath = bIsDebug ?
		GetCachedShaderWithDebugFilepath((*it).m_fileId, (*it).m_permutation, ShaderCache::FragmentShaderTag) :
		GetCachedShaderFilepath((*it).m_fileId, (*it).m_permutation, ShaderCache::FragmentShaderTag);

	std::filesystem::path computeFilepath = bIsDebug ?
		GetCachedShaderWithDebugFilepath((*it).m_fileId, (*it).m_permutation, ShaderCache::ComputeShaderTag) :
		GetCachedShaderFilepath((*it).m_fileId, (*it).m_permutation, ShaderCache::ComputeShaderTag);

	AssetRegistry::ReadBinaryFile(vertexFilepath, vertexSpirv);
	AssetRegistry::ReadBinaryFile(fragmentFilepath, fragmentSpirv);
	AssetRegistry::ReadBinaryFile(computeFilepath, computeSpirv);

	return true;
}

bool ShaderCache::IsExpired(const FileId& uid, uint32_t permutation) const
{
	if (!Contains(uid))
	{
		return true;
	}

	const auto& entries = m_cache.m_data[uid];
	size_t index = entries.FindIf([permutation](const ShaderCacheData::Entry& arg) { return arg.m_permutation == permutation; });

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

	return hasAsset ? entries[index].m_timestamp < timeStamp : true;
}

bool ShaderCache::GetTimeStamp(const FileId& uid, time_t& outTimestamp) const
{
	if (ShaderAssetInfoPtr assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<ShaderAssetInfoPtr>(uid))
	{
		if (TSharedPtr<ShaderAsset> shaderAsset = App::GetSubmodule<ShaderCompiler>()->LoadShaderAsset(assetInfo->GetFileId()).Lock())
		{
			outTimestamp = assetInfo->GetAssetLastModificationTime();
			for (const auto& include : shaderAsset->GetIncludes())
			{
				outTimestamp = std::max(outTimestamp, Sailor::Utils::GetFileModificationTime(AssetRegistry::GetContentFolder() + include));
			}
			return true;
		}
	}
	return false;
}