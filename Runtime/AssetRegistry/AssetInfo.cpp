#include "AssetRegistry/AssetInfo.h"
#include "AssetRegistry/AssetRegistry.h"
#include <corecrt_io.h>
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include <iostream>

using namespace Sailor;

void AssetInfo::Serialize(nlohmann::json& outData) const
{
	m_UID.Serialize(outData["uid"]);
	outData["filename"] = std::filesystem::path(GetAssetFilepath()).filename().string();
	outData["assetImportTime"] = m_assetImportTime;
}

void AssetInfo::Deserialize(const nlohmann::json& inData)
{
	m_UID.Deserialize(inData["uid"]);
	m_assetFilename = inData["filename"].get<std::string>();
	m_assetImportTime = inData["assetImportTime"].get<std::time_t>();
}

void AssetInfo::SaveMetaFile()
{
	std::ofstream assetFile{ GetMetaFilepath() };

	json newMeta;
	Serialize(newMeta);
	assetFile << newMeta.dump(Sailor::JsonDumpIndent);
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
	Utils::Erase(res, AssetRegistry::ContentRootFolder);
	return res;
}

std::string AssetInfo::GetRelativeMetaFilepath() const
{
	std::string res = GetMetaFilepath();
	Utils::Erase(res, AssetRegistry::ContentRootFolder);
	return res;
}

AssetInfoPtr IAssetInfoHandler::ImportAsset(const std::string& assetFilepath) const
{
	const std::string assetInfoFilename = AssetRegistry::GetMetaFilePath(assetFilepath);
	std::filesystem::remove(assetInfoFilename);
	std::ofstream assetFile{ assetInfoFilename };

	json newMeta;
	GetDefaultMetaJson(newMeta);
	UID::CreateNewUID().Serialize(newMeta["uid"]);

	newMeta["filename"] = std::filesystem::path(assetFilepath).filename().string();
	newMeta["assetImportTime"] = std::time(nullptr);

	assetFile << newMeta.dump(Sailor::JsonDumpIndent);
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

	std::ifstream assetFile(assetInfo->GetMetaFilepath());

	json meta;
	assetFile >> meta;

	assetInfo->Deserialize(meta);
	assetInfo->m_metaLoadTime = std::time(nullptr);
	
	const bool bWasAssetExpired = assetInfo->IsAssetExpired();

	assetInfo->m_assetImportTime = assetInfo->GetAssetLastModificationTime();

	assetFile.close();

	for (IAssetInfoHandlerListener* listener : m_listeners)
	{
		listener->OnUpdateAssetInfo(assetInfo, bWasMetaExpired || bWasAssetExpired);
	}

	if (bWasAssetExpired)
	{
		assetInfo->SaveMetaFile();
	}
}

void DefaultAssetInfoHandler::GetDefaultMetaJson(nlohmann::json& outDefaultJson) const
{
	AssetInfo defaultObject;
	defaultObject.Serialize(outDefaultJson);
}

AssetInfoPtr DefaultAssetInfoHandler::CreateAssetInfo() const
{
	return new AssetInfo();
}
