#include "AssetInfo.h"
#include "AssetRegistry.h"
#include <combaseapi.h>
#include <corecrt_io.h>
#include <filesystem>
#include <fstream>
#include "Utils.h"
#include <iostream>

using namespace Sailor;

void ns::to_json(json& j, const Sailor::AssetInfo& p)
{
	ns::to_json(j["uid"], p.GetUID());
	j["filepath"] = p.GetAssetFilepath();
}

void ns::from_json(const json& j, Sailor::AssetInfo& p)
{
	ns::from_json(j["uid"], p.uid);
	p.m_filepath = j["filepath"].get<std::string>();
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

void DefaultAssetInfoHandler::Serialize(const AssetInfo* inInfo, json& outData) const
{
	ns::to_json(outData, *inInfo);
}

void DefaultAssetInfoHandler::Deserialize(const json& inData, AssetInfo* outInfo) const
{
	ns::from_json(inData, *outInfo);
}

AssetInfo* DefaultAssetInfoHandler::ImportFile(const std::string& filepath) const
{
	std::cout << "Try import file: " << filepath << std::endl;

	const std::string assetInfofilepath = Utils::RemoveExtension(filepath) + AssetRegistry::MetaFileExtension;
	std::filesystem::remove(assetInfofilepath);
	std::ofstream assetFile{ assetInfofilepath };

	json newMeta;
	AssetInfo defaultObject;
		
	ns::to_json(newMeta, defaultObject);
	ns::to_json(newMeta["uid"], UID::CreateNewUID());
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
	Deserialize(meta, res);

	assetFile.close();

	return res;
}
