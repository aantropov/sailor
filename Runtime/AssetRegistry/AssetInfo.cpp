#include "AssetRegistry/AssetInfo.h"
#include "AssetRegistry/AssetRegistry.h"
#include <corecrt_io.h>
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include <iostream>

using namespace Sailor;

YAML::Node AssetInfo::Serialize() const
{
	YAML::Node outData;
	const std::string filename = std::filesystem::path(GetAssetFilepath()).filename().string();

	outData["fileId"] = m_fileId;
	outData["filename"] = filename;
	return outData;
}

void AssetInfo::Deserialize(const YAML::Node& inData)
{
	m_fileId = inData["fileId"].as<FileId>();
	m_assetFilename = inData["filename"].as<std::string>();
}

void AssetInfo::SaveMetaFile()
{
	std::ofstream assetFile{ GetMetaFilepath() };

	YAML::Node node = Serialize();
	assetFile << node;
	assetFile.close();

	m_metaLoadTime = GetMetaLastModificationTime();
}

AssetInfo::AssetInfo()
{
	m_metaLoadTime = std::time(nullptr);
	m_assetImportTime = 0;
}

bool AssetInfo::IsAssetExpired() const
{
	if (!(std::filesystem::exists(m_folder)))
	{
		return true;
	}

	return m_assetImportTime < GetAssetLastModificationTime();
}

bool AssetInfo::IsMetaExpired() const
{
	if (!(std::filesystem::exists(m_folder)))
	{
		return true;
	}

	return m_metaLoadTime < GetMetaLastModificationTime();
}

DefaultAssetInfoHandler::DefaultAssetInfoHandler(AssetRegistry* assetRegistry)
{
	assetRegistry->RegisterAssetInfoHandler(m_supportedExtensions, this);
}

std::time_t AssetInfo::GetAssetLastModificationTime() const
{
	return Sailor::Utils::GetFileModificationTime(GetAssetFilepath());
}

std::time_t AssetInfo::GetAssetImportTime() const
{
	return m_assetImportTime;
}

std::time_t AssetInfo::GetMetaLastModificationTime() const
{
	return Sailor::Utils::GetFileModificationTime(GetMetaFilepath());
}

std::string AssetInfo::GetMetaFilepath() const
{
	return m_folder + m_assetFilename + "." + AssetRegistry::MetaFileExtension;
}

std::string AssetInfo::GetRelativeAssetFilepath() const
{
	std::string res = GetAssetFilepath();
	Utils::Erase(res, AssetRegistry::GetContentFolder());
	return res;
}

std::string AssetInfo::GetRelativeMetaFilepath() const
{
	std::string res = GetMetaFilepath();
	Utils::Erase(res, AssetRegistry::GetContentFolder());
	return res;
}

AssetInfoPtr IAssetInfoHandler::ImportAsset(const std::string& assetFilepath) const
{
	const std::string assetInfoFilename = AssetRegistry::GetMetaFilePath(assetFilepath);
	std::filesystem::remove(assetInfoFilename);
	std::ofstream assetFile{ assetInfoFilename };

	YAML::Node newMeta;
	GetDefaultMeta(newMeta);

	auto fileId = FileId::CreateNewFileId();
	newMeta["fileId"] = fileId.Serialize();
	newMeta["filename"] = std::filesystem::path(assetFilepath).filename().string();

	App::GetSubmodule<AssetRegistry>()->CacheAssetTime(fileId, std::time(nullptr));

	assetFile << newMeta;
	assetFile.close();

	auto assetInfoPtr = LoadAssetInfo(assetInfoFilename);
	for (IAssetInfoHandlerListener* listener : m_listeners)
	{
		listener->OnImportAsset(assetInfoPtr);
	}

	assetInfoPtr->SaveMetaFile();

	return assetInfoPtr;
}

AssetInfoPtr IAssetInfoHandler::LoadAssetInfo(const std::string& assetInfoPath) const
{
	AssetInfoPtr res = CreateAssetInfo();
	res->m_folder = std::filesystem::path(assetInfoPath).remove_filename().string();

	// Temp to pass asset filename to Reload Asset Info
	const std::string filename = std::filesystem::path(assetInfoPath).filename().string();
	res->m_assetFilename = filename.substr(0, filename.length() - strlen(AssetRegistry::MetaFileExtension) - 1);

	ReloadAssetInfo(res);

	return res;
}

void IAssetInfoHandler::ReloadAssetInfo(AssetInfoPtr assetInfo) const
{
	const bool bWasMetaExpired = assetInfo->IsMetaExpired();

	std::string content;
	AssetRegistry::ReadAllTextFile(assetInfo->GetMetaFilepath(), content);

	YAML::Node meta = YAML::Load(content);

	assetInfo->Deserialize(meta);
	assetInfo->m_metaLoadTime = std::time(nullptr);

	App::GetSubmodule<AssetRegistry>()->GetAssetCachedTime(assetInfo->GetFileId(), assetInfo->m_assetImportTime);

	const bool bWasAssetExpired = assetInfo->IsAssetExpired();

	assetInfo->m_assetImportTime = assetInfo->GetAssetLastModificationTime();
	App::GetSubmodule<AssetRegistry>()->CacheAssetTime(assetInfo->GetFileId(), assetInfo->m_assetImportTime);

	for (IAssetInfoHandlerListener* listener : m_listeners)
	{
		listener->OnUpdateAssetInfo(assetInfo, bWasMetaExpired || bWasAssetExpired);
	}

	/*	if (bWasAssetExpired)
		{
			assetInfo->SaveMetaFile();
		}
		*/
}

void DefaultAssetInfoHandler::GetDefaultMeta(YAML::Node& outDefaultYaml) const
{
	AssetInfo defaultObject;
	outDefaultYaml = defaultObject.Serialize();
}

AssetInfoPtr DefaultAssetInfoHandler::CreateAssetInfo() const
{
	return new AssetInfo();
}
