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
	outData["filepath"] = GetAssetFilepath();
}

void AssetInfo::Deserialize(const nlohmann::json& inData)
{
	m_UID.Deserialize(inData["uid"]);
	m_filepath = inData["filepath"].get<std::string>();
}

AssetInfo::AssetInfo()
{
	m_creationTime = std::time(nullptr);
}

void DefaultAssetInfoHandler::Initialize()
{
	m_pInstance = new DefaultAssetInfoHandler();
	AssetRegistry::GetInstance()->RegisterAssetInfoHandler(m_pInstance->m_supportedExtensions, m_pInstance);
}

AssetInfo* DefaultAssetInfoHandler::ImportFile(const std::string& filepath) const
{
	std::cout << "Try import file: " << filepath << std::endl;

	const std::string assetInfofilepath = Utils::RemoveExtension(filepath) + AssetRegistry::MetaFileExtension;
	std::filesystem::remove(assetInfofilepath);
	std::ofstream assetFile{ assetInfofilepath };

	json newMeta;
	AssetInfo defaultObject;

	defaultObject.Serialize(newMeta);
	UID::CreateNewUID().Serialize(newMeta["uid"]);

	newMeta["filepath"] = filepath;

	assetFile << newMeta.dump();
	assetFile.close();

	return ImportAssetInfo(assetInfofilepath);
}

AssetInfo* DefaultAssetInfoHandler::ImportAssetInfo(const std::string& assetInfoPath) const
{
	std::cout << "Try load asset info: " << assetInfoPath << std::endl;

	AssetInfo* res = new AssetInfo();

	std::ifstream assetFile(assetInfoPath);

	json meta;
	assetFile >> meta;

	res->Deserialize(meta);

	assetFile.close();

	return res;
}
