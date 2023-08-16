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

	ScanFolder(ContentRootFolder);
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

const FileId& AssetRegistry::GetOrLoadAsset(const std::string& assetFilepath)
{
	if (auto assetInfo = GetAssetInfoPtr(assetFilepath))
	{
		return assetInfo->GetFileId();
	}
	return LoadAsset(assetFilepath);
}

const FileId& AssetRegistry::LoadAsset(const std::string& assetFilepath)
{
	// Convert to absolute path
	const std::string filepath = (!assetFilepath._Starts_with(ContentRootFolder)) ?
		(ContentRootFolder + Utils::SanitizeFilepath(assetFilepath)) :
		Utils::SanitizeFilepath(assetFilepath);

	const std::string extension = Utils::GetFileExtension(filepath);

	if (extension != MetaFileExtension)
	{
		const std::string assetInfoFile = GetMetaFilePath(filepath);
		IAssetInfoHandler* assetInfoHandler = App::GetSubmodule<DefaultAssetInfoHandler>();

		auto assetInfoHandlerIt = m_assetInfoHandlers.Find(extension);
		if (assetInfoHandlerIt != m_assetInfoHandlers.end())
		{
			assetInfoHandler = (*assetInfoHandlerIt).m_second;
		}

		check(assetInfoHandler);

		auto uid = m_fileIds.Find(filepath);
		if (uid != m_fileIds.end())
		{
			auto assetInfoIt = m_loadedAssetInfo.Find(uid->m_second);
			if (assetInfoIt != m_loadedAssetInfo.end())
			{
				AssetInfoPtr assetInfo = assetInfoIt->m_second;
				if (assetInfo->IsMetaExpired() || assetInfo->IsAssetExpired())
				{
					SAILOR_LOG("Reload asset info: %s", assetInfoFile.c_str());
					assetInfoHandler->ReloadAssetInfo(assetInfo);
				}
				return (*uid).m_second;
			}

			// Meta were delete
			m_fileIds.Remove(uid->m_first);
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
			LoadAsset(entry.path().string());
		}
	}
}

AssetInfoPtr AssetRegistry::GetAssetInfoPtr_Internal(FileId uid) const
{
	SAILOR_PROFILE_FUNCTION();

	auto it = m_loadedAssetInfo.Find(uid);
	if (it != m_loadedAssetInfo.end())
	{
		return it->m_second;
	}
	return nullptr;
}

AssetInfoPtr AssetRegistry::GetAssetInfoPtr_Internal(const std::string& assetFilepath) const
{
	auto it = m_fileIds.Find(ContentRootFolder + assetFilepath);
	if (it != m_fileIds.end())
	{
		return GetAssetInfoPtr_Internal(it->m_second);
	}

	return nullptr;
}

AssetRegistry::~AssetRegistry()
{
	for (auto& asset : m_loadedAssetInfo)
	{
		delete asset.m_second;
	}
}