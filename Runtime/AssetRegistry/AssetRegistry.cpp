#include "AssetRegistry/AssetRegistry.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include "Containers/Map.h"
#include "AssetRegistry/AssetInfo.h"
#include "AssetRegistry/Shader/ShaderAssetInfo.h"
#include "AssetRegistry/Texture/TextureAssetInfo.h"
#include "AssetRegistry/Model/ModelAssetInfo.h"
#include "AssetRegistry/Material/MaterialAssetInfo.h"
#include "Core/Utils.h"
#include "nlohmann_json/include/nlohmann/json.hpp"
#include "Tasks/Tasks.h"
#include "Tasks/Scheduler.h"

using namespace Sailor;
using namespace nlohmann;

AssetRegistry::AssetRegistry()
{
	m_assetCache.Initialize();
}

void AssetRegistry::CacheAssetTime(const FileId& id, const time_t& assetTimestamp)
{
	m_assetCache.Update(id, assetTimestamp);
	m_assetCache.SaveCache();
}

bool AssetRegistry::GetAssetCachedTime(const FileId& id, time_t& outAssetTimestamp) const
{
	return m_assetCache.GetTimeStamp(id, outAssetTimestamp);
}

bool AssetRegistry::IsAssetExpired(const AssetInfoPtr info) const
{
	return m_assetCache.IsExpired(info);
}

bool AssetRegistry::ReadAllTextFile(const std::string& filename, std::string& text)
{
	SAILOR_PROFILE_FUNCTION();

	constexpr auto readSize = std::size_t{ 4096 };
	auto stream = std::ifstream{ filename.data() };
	stream.exceptions(std::ios_base::badbit);

	if (!stream.is_open())
	{
		return false;
	}

	text = std::string{};
	auto buf = std::string(readSize, '\0');
	while (stream.read(&buf[0], readSize))
	{
		text.append(buf, 0, stream.gcount());
	}
	text.append(buf, 0, stream.gcount());

	return true;
}

void AssetRegistry::ScanContentFolder()
{
	SAILOR_PROFILE_FUNCTION();

	ScanFolder(GetContentFolder());
}

bool AssetRegistry::RegisterAssetInfoHandler(const TVector<std::string>& supportedExtensions, IAssetInfoHandler* assetInfoHandler)
{
	SAILOR_PROFILE_FUNCTION();

	bool bAssigned = false;
	for (const auto& extension : supportedExtensions)
	{
		if (m_assetInfoHandlers.ContainsKey(extension))
		{
			continue;
		}

		m_assetInfoHandlers[extension] = assetInfoHandler;
		bAssigned = true;
	}

	return bAssigned;
}

std::string AssetRegistry::GetMetaFilePath(const std::string& assetFilepath)
{
	return assetFilepath + "." + MetaFileExtension;
}

const FileId& AssetRegistry::GetOrLoadFile(const std::string& assetFilepath)
{
	if (auto assetInfo = GetAssetInfoPtr(assetFilepath))
	{
		return assetInfo->GetFileId();
	}
	return LoadFile(assetFilepath);
}

const FileId& AssetRegistry::LoadFile(const std::string& assetFilepath)
{
	// Convert to absolute path
	const std::string filepath = (!assetFilepath._Starts_with(GetContentFolder())) ?
		(GetContentFolder() + Utils::SanitizeFilepath(assetFilepath)) :
		Utils::SanitizeFilepath(assetFilepath);

	const std::string extension = Utils::GetFileExtension(filepath);

	if (extension != MetaFileExtension)
	{
		const std::string assetInfoFile = GetMetaFilePath(filepath);
		IAssetInfoHandler* assetInfoHandler = App::GetSubmodule<DefaultAssetInfoHandler>();

		auto assetInfoHandlerIt = m_assetInfoHandlers.Find(extension);
		if (assetInfoHandlerIt != m_assetInfoHandlers.end())
		{
			assetInfoHandler = *(*assetInfoHandlerIt).m_second;
		}

		check(assetInfoHandler);

		auto uid = m_fileIds.Find(filepath);
		if (uid != m_fileIds.end())
		{
			auto assetInfoIt = m_loadedAssetInfo.Find(uid.Value());
			if (assetInfoIt != m_loadedAssetInfo.end())
			{
				AssetInfoPtr assetInfo = assetInfoIt.Value();
				if (assetInfo->IsMetaExpired() || assetInfo->IsAssetExpired())
				{
					SAILOR_LOG("Reload asset info: %s", assetInfoFile.c_str());
					assetInfoHandler->ReloadAssetInfo(assetInfo);
				}
				return uid.Value();
			}

			// Meta were delete
			m_fileIds.Remove(uid.Key());
		}

		AssetInfoPtr assetInfo = nullptr;
		if (std::filesystem::exists(assetInfoFile))
		{
			//SAILOR_LOG("Load asset info: %s", assetInfoFile.c_str());
			assetInfo = assetInfoHandler->LoadAssetInfo(assetInfoFile);
		}
		else
		{
			SAILOR_LOG("Import new asset: %s", filepath.c_str());
			assetInfo = assetInfoHandler->ImportAsset(filepath);
		}

		m_loadedAssetInfo[assetInfo->GetFileId()] = assetInfo;
		m_fileIds[filepath] = assetInfo->GetFileId();

		return assetInfo->GetFileId();
	}

	return FileId::Invalid;
}

void AssetRegistry::ScanFolder(const std::string& folderPath)
{
	SAILOR_PROFILE_FUNCTION();

	for (const auto& entry : std::filesystem::directory_iterator(folderPath))
	{
		if (entry.is_directory())
		{
			ScanFolder(entry.path().string());
		}
		else if (entry.is_regular_file())
		{
			GetOrLoadFile(entry.path().string());
		}
	}
}

TObjectPtr<Object> AssetRegistry::LoadAsset(IAssetInfoHandler* assetInfoHandler, const FileId& id, bool bImmediate)
{
	TObjectPtr<Object> out;
	assetInfoHandler->GetFactory()->LoadAsset(id, out, bImmediate);
	return out;
}

AssetInfoPtr AssetRegistry::GetAssetInfoPtr_Internal(FileId uid) const
{
	SAILOR_PROFILE_FUNCTION();

	auto it = m_loadedAssetInfo.Find(uid);
	if (it != m_loadedAssetInfo.end())
	{
		return it.Value();
	}
	return nullptr;
}

AssetInfoPtr AssetRegistry::GetAssetInfoPtr_Internal(const std::string& assetFilepath) const
{
	auto it = m_fileIds.Find(GetContentFolder() + assetFilepath);
	if (it != m_fileIds.end())
	{
		return GetAssetInfoPtr_Internal(it.Value());
	}

	return nullptr;
}

AssetRegistry::~AssetRegistry()
{
	m_assetCache.Shutdown();

	for (const auto& assetIt : m_loadedAssetInfo)
	{
		delete* assetIt.m_second;
	}
}