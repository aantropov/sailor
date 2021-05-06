#include "ShaderAssetInfo.h"
#include "AssetInfo.h"
#include <filesystem>
#include <fstream>
#include "Utils.h"
#include <iostream>

using namespace Sailor;

void ShaderAssetInfoHandler::Initialize()
{
	m_pInstance = new ShaderAssetInfoHandler();

	m_pInstance->m_supportedExtensions.emplace_back("shader");
	AssetRegistry::GetInstance()->RegisterAssetInfoHandler(m_pInstance->m_supportedExtensions, m_pInstance);
}

ShaderAssetInfo* ShaderAssetInfoHandler::ImportFile(const std::string& filepath) const
{
	std::cout << "Try import file: " << filepath << std::endl;

	const std::string assetInfofilepath = Utils::RemoveExtension(filepath) + AssetRegistry::MetaFileExtension;
	std::filesystem::remove(assetInfofilepath);
	std::ofstream assetFile{ assetInfofilepath };

	json newMeta;
	ShaderAssetInfo defaultObject;

	defaultObject.Serialize(newMeta);
	UID::CreateNewUID().Serialize(newMeta["uid"]);
	newMeta["filepath"] = filepath;

	assetFile << newMeta.dump();
	assetFile.close();

	return ImportAssetInfo(assetInfofilepath);
}

ShaderAssetInfo* ShaderAssetInfoHandler::ImportAssetInfo(const std::string& assetInfoPath) const
{
	std::cout << "Try load asset info: " << assetInfoPath << std::endl;

	ShaderAssetInfo* res = new ShaderAssetInfo();
	std::ifstream assetFile(assetInfoPath);

	json meta;
	assetFile >> meta;
	res->Deserialize(meta);

	assetFile.close();

	return res;
}
