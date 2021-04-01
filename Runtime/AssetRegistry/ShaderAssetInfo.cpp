#include "ShaderAssetInfo.h"
#include "AssetInfo.h"
#include <filesystem>
#include <fstream>
#include "Utils.h"
#include <iostream>

using namespace Sailor;

void ns::to_json(json& j, const Sailor::ShaderAssetInfo& p)
{
	ns::to_json(j,  static_cast<const Sailor::AssetInfo&>(p));
}

void ns::from_json(const json& j, Sailor::ShaderAssetInfo& p)
{
	ns::from_json(j, static_cast<Sailor::AssetInfo&>(p));
}

void ShaderAssetInfoHandler::Initialize()
{
	instance = new ShaderAssetInfoHandler();

	instance->supportedExtensions.push_back("shader");
	AssetRegistry::GetInstance()->RegisterAssetInfoHandler(instance->supportedExtensions, instance);
}

void ShaderAssetInfoHandler::Serialize(const AssetInfo* inInfo, json& outData) const
{
	DefaultAssetInfoHandler::GetInstance()->Serialize(inInfo, outData);
	//ns::to_json(outData, dynamic_cast<const ShaderAssetInfo&>(*inInfo));
}

void ShaderAssetInfoHandler::Deserialize(const json& inData, AssetInfo* outInfo) const
{
	DefaultAssetInfoHandler::GetInstance()->Deserialize(inData, outInfo);
	//ns::from_json(inData, dynamic_cast<ShaderAssetInfo&>(*outInfo));
}

ShaderAssetInfo* ShaderAssetInfoHandler::ImportFile(const std::string& filePath) const
{
	std::cout << "Try import file: " << filePath << std::endl;

	const std::string assetInfoFilePath = Utils::RemoveExtension(filePath) + AssetRegistry::MetaFileExtension;
	std::filesystem::remove(assetInfoFilePath);
	std::ofstream assetFile{ assetInfoFilePath };

	json newMeta;
	ShaderAssetInfo defaultObject;

	ns::to_json(newMeta, defaultObject);
	ns::to_json(newMeta["uid"], UID::CreateNewUID());

	assetFile << newMeta.dump();
	assetFile.close();

	return ImportAssetInfo(assetInfoFilePath);
}

ShaderAssetInfo* ShaderAssetInfoHandler::ImportAssetInfo(const std::string& assetInfoPath) const
{
	std::cout << "Try load asset info: " << assetInfoPath << std::endl;

	ShaderAssetInfo* res = new ShaderAssetInfo();

	std::ifstream assetFile(assetInfoPath);

	json meta;
	assetFile >> meta;
	Deserialize(meta, res);

	assetFile.close();

	return res;
}
