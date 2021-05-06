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
	m_filename = inData["filename"].get<std::string>();
}

AssetInfo::AssetInfo()
{
	m_loadTime = std::time(nullptr);
}

void DefaultAssetInfoHandler::Initialize()
{
	m_pInstance = new DefaultAssetInfoHandler();
	AssetRegistry::GetInstance()->RegisterAssetInfoHandler(m_pInstance->m_supportedExtensions, m_pInstance);
}

AssetInfo* IAssetInfoHandler::ImportFile(const std::string& filepath) const
{
	std::cout << "Try import file: " << filepath << std::endl;

	const std::string assetInfofilepath = Utils::RemoveExtension(filepath) + AssetRegistry::MetaFileExtension;
	std::filesystem::remove(assetInfofilepath);
	std::ofstream assetFile{ assetInfofilepath };

	json newMeta;
	GetDefaultAssetInfoMeta(newMeta);
	UID::CreateNewUID().Serialize(newMeta["uid"]);
		
	newMeta["filename"] = std::filesystem::path(filepath).filename().string();

	assetFile << newMeta.dump();
	assetFile.close();
	
	return LoadAssetInfo(assetInfofilepath);
}

AssetInfo* IAssetInfoHandler::LoadAssetInfo(const std::string& assetInfoPath) const
{
	std::cout << "Try load asset info: " << assetInfoPath << std::endl;

	AssetInfo* res = CreateAssetInfo();

	std::ifstream assetFile(assetInfoPath);

	json meta;
	assetFile >> meta;
		
	res->Deserialize(meta);
	res->m_path = std::filesystem::path(assetInfoPath).remove_filename().string();
	res->m_loadTime = std::time(nullptr);
	
	assetFile.close();

	return res;
}

void DefaultAssetInfoHandler::GetDefaultAssetInfoMeta(nlohmann::json& outDefaultJson) const
{
	AssetInfo defaultObject;
	defaultObject.Serialize(outDefaultJson);
}

AssetInfo* DefaultAssetInfoHandler::CreateAssetInfo() const
{
	return new AssetInfo();
}