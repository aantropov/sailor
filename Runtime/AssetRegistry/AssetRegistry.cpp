#include "AssetRegistry.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include "AssetInfo.h"
#include "Utils.h"
#include "nlohmann_json/include/nlohmann/json.hpp"

using namespace Sailor;
using namespace nlohmann;

const std::string AssetRegistry::ContentRootFolder = "..//Content//";
const std::string AssetRegistry::MetaFileExtension = ".asset";

void AssetRegistry::Initialize()
{
	instance = new AssetRegistry();
	instance->LoadAll();
}

bool AssetRegistry::ReadFile(const std::string& filename, std::vector<char>& buffer)
{
	std::ifstream file(filename, std::ios::ate | std::ios::binary);

	if (!file.is_open())
	{
		return false;
	}

	size_t fileSize = (size_t)file.tellg();
	buffer.clear();
	buffer.reserve(fileSize);

	file.seekg(0);
	file >> buffer.data();

	file.close();
	return true;
}

void AssetRegistry::ScanContentFolder()
{
	ScanFolder(ContentRootFolder);
}

void AssetRegistry::LoadAll()
{
	LoadAssetsInFolder(ContentRootFolder);
}

void AssetRegistry::LoadAssetsInFolder(const std::string& folderPath)
{
	for (const auto& entry : std::filesystem::directory_iterator(folderPath))
	{
		if (entry.is_directory())
		{
			LoadAssetsInFolder(entry.path().string());
		}
		else if (entry.is_regular_file())
		{
			LoadAsset(entry.path().string(), true);
		}
	}
}

bool AssetRegistry::LoadAsset(const std::string& filePath, bool bShouldImport)
{
	std::cout << "Try load asset: " << filePath << std::endl;

	const std::string assetFilePath = Utils::RemoveExtension(filePath) + MetaFileExtension;
	const std::string extension = Utils::GetExtension(filePath);

	if (extension == MetaFileExtension)
	{
		return false;
	}

	if (!std::filesystem::exists(assetFilePath) && bShouldImport)
	{
		if (!ImportFile(filePath))
		{
			return false;
		}
	}

	if (std::filesystem::exists(assetFilePath))
	{
		//TODO: Load asset				
		/*auto assetToLoad = CreateAsset(GetAssetTypeByExtension(extension));

		std::ifstream assetFile(assetFilePath);

		json meta;
		assetFile >> meta;

		assetToLoad->Deserialize(meta);
		assetToLoad->Load(assetFilePath);

		assetFile.close();

		loadedAssets[assetToLoad->Getuid()] = assetToLoad;
		*/
		return true;
	}

	return false;
}

bool AssetRegistry::ImportFile(const std::string& filename)
{
	/*
	const std::string assetFilePath = RemoveExtension(filename) + MetaFileExtension;
	std::filesystem::remove(assetFilePath);
	std::ofstream assetFile{ assetFilePath };

	::uid uid;
	HRESULT hr = CoCreateuid(&uid);

	char buffer[128];
	sprintf_s(buffer, "{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
		uid.Data1, uid.Data2, uid.Data3,
		uid.Data4[0], uid.Data4[1], uid.Data4[2], uid.Data4[3],
		uid.Data4[4], uid.Data4[5], uid.Data4[6], uid.Data4[7]);

	json newMeta = {
		{"uid", buffer}
	};

	assetFile << newMeta.dump();
	assetFile.close();
	*/
	return true;
}

bool AssetRegistry::RegisterAssetInfoHandler(const std::vector<std::string>& supportedExtensions, IAssetInfoHandler* assetInfoHandler)
{
	bool bAssigned = false;
	for (const auto& extension : supportedExtensions)
	{
		if (assetInfoHandlers.find(extension) != assetInfoHandlers.end())
		{
			continue;
		}

		assetInfoHandlers[extension] = assetInfoHandler;
		bAssigned = true;
	}

	return bAssigned;
}

void AssetRegistry::ScanFolder(const std::string& folderPath)
{
	for (const auto& entry : std::filesystem::directory_iterator(folderPath))
	{
		if (entry.is_directory())
		{
			ScanFolder(entry.path().string());
		}
		else if (entry.is_regular_file())
		{
			if (Utils::GetExtension(entry.path().string()) == "asset")
			{

			}
		}
	}
}

AssetRegistry::~AssetRegistry()
{
	for (auto& asset : loadedAssets)
	{
		delete asset.second;
	}
}