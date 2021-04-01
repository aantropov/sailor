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
}

void ns::from_json(const json& j, Sailor::AssetInfo& p)
{
	ns::from_json(j["uid"], p.uid);
}

AssetInfo::AssetInfo()
{
	creationTime = std::time(nullptr);
}

void DefaultAssetInfoHandler::Initialize()
{
	instance = new DefaultAssetInfoHandler();
	AssetRegistry::GetInstance()->RegisterAssetInfoHandler(instance->supportedExtensions, instance);
}

void DefaultAssetInfoHandler::Serialize(const AssetInfo* inInfo, json& outData) const
{
	ns::to_json(outData, *inInfo);
}

void DefaultAssetInfoHandler::Deserialize(const json& inData, AssetInfo* outInfo) const
{
	ns::from_json(inData, *outInfo);
}

AssetInfo* DefaultAssetInfoHandler::ImportFile(const std::string& filePath) const
{
	std::cout << "Try import file: " << filePath << std::endl;

	const std::string assetInfoFilePath = Utils::RemoveExtension(filePath) + AssetRegistry::MetaFileExtension;
	std::filesystem::remove(assetInfoFilePath);
	std::ofstream assetFile{ assetInfoFilePath };

	json newMeta;
	AssetInfo defaultObject;
		
	ns::to_json(newMeta, defaultObject);
	ns::to_json(newMeta["uid"], UID::CreateNewUID());

	assetFile << newMeta.dump();
	assetFile.close();

	return ImportAssetInfo(assetInfoFilePath);
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
