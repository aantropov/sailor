#include "AssetInfo.h"
#include "AssetRegistry.h"
#include <combaseapi.h>
#include <corecrt_io.h>
#include <filesystem>
#include <fstream>
#include "Utils.h"
#include <iostream>

using namespace Sailor;

void AssetInfo::Serialize(nlohmann::json& outData) const
{
	m_UID.Serialize(outData["uid"]);
	outData["filename"] = GetAssetFilepath();
}

void AssetInfo::Deserialize(const nlohmann::json& inData)
{
	m_UID.Deserialize(inData["uid"]);
	m_assetFilename = inData["filename"].get<std::string>();
}

AssetInfo::AssetInfo()
{
	m_loadTime = std::time(nullptr);
}


bool AssetInfo::IsExpired() const
{
	if (!(std::filesystem::exists(m_folder)))
	{
		return true;
	}

	return m_loadTime < GetMetaLastModificationTime();
}


void DefaultAssetInfoHandler::Initialize()
{
	m_pInstance = new DefaultAssetInfoHandler();
	AssetRegistry::GetInstance()->RegisterAssetInfoHandler(m_pInstance->m_supportedExtensions, m_pInstance);
}

std::time_t AssetInfo::GetAssetLastModificationTime() const
{
	return Sailor::Utils::GetFileModificationTime(GetAssetFilepath());
}

std::time_t AssetInfo::GetMetaLastModificationTime() const
{
	return Sailor::Utils::GetFileModificationTime(GetMetaFilepath());
}

std::string AssetInfo::GetMetaFilepath() const
{
	return Utils::RemoveFileExtension(m_assetFilename) + AssetRegistry::MetaFileExtension;
}

AssetInfo* IAssetInfoHandler::ImportAsset(const std::string& assetFilepath) const
{
	const std::string assetInfoFilename = Utils::RemoveFileExtension(assetFilepath) + AssetRegistry::MetaFileExtension;
	std::filesystem::remove(assetInfoFilename);
	std::ofstream assetFile{ assetInfoFilename };

	json newMeta;
	GetDefaultMetaJson(newMeta);
	UID::CreateNewUID().Serialize(newMeta["uid"]);
		
	newMeta["filename"] = std::filesystem::path(assetFilepath).filename().string();

	assetFile << newMeta.dump();
	assetFile.close();
	
	return LoadAssetInfo(assetInfoFilename);
}

AssetInfo* IAssetInfoHandler::LoadAssetInfo(const std::string& assetInfoPath) const
{	
	AssetInfo* res = CreateAssetInfo();
	res->m_folder = std::filesystem::path(assetInfoPath).remove_filename().string();
	// Temp to pass asset filename to Reload Asset Info
	res->m_assetFilename = std::filesystem::path(assetInfoPath).string();

	ReloadAssetInfo(res);

	return res;
}

void IAssetInfoHandler::ReloadAssetInfo(AssetInfo* assetInfo) const
{
	std::ifstream assetFile(assetInfo->GetMetaFilepath());
	
	json meta;
	assetFile >> meta;

	assetInfo->Deserialize(meta);
	assetInfo->m_loadTime = std::time(nullptr);

	assetFile.close();
}

void DefaultAssetInfoHandler::GetDefaultMetaJson(nlohmann::json& outDefaultJson) const
{
	AssetInfo defaultObject;
	defaultObject.Serialize(outDefaultJson);
}

AssetInfo* DefaultAssetInfoHandler::CreateAssetInfo() const
{
	return new AssetInfo();
}