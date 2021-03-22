#include "AssetRegistry.h"
#include <combaseapi.h>
#include <corecrt_io.h>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace Sailor;

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

void AssetRegistry::LoadAll()
{
	LoadAssetsInFolder(ContentRootFolder);
}

std::string removeExtension(const std::string& filename)
{
	size_t lastdot = filename.find_last_of(".");
	if (lastdot == std::string::npos)
		return filename;
	return filename.substr(0, lastdot);
}

std::string getExtension(const std::string& filename)
{
	size_t lastdot = filename.find_last_of(".");
	if (lastdot == std::string::npos)
		return std::string();
	return filename.substr(lastdot, filename.size() - lastdot);
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

	const std::string assetFilePath = removeExtension(filePath) + MetaFileExtension;
	const std::string extension = getExtension(filePath);

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
		auto assetToLoad = CreateAsset(GetAssetTypeByExtension(extension));

		std::ifstream assetFile(assetFilePath);

		json meta;
		assetFile >> meta;
		
		assetToLoad->Deserialize(meta);

		auto a = meta["guid"].get<std::string>();
		
		assetToLoad->Load();

		assets[assetToLoad->guid] = assetToLoad;

		return true;
	}

	return false;
}

bool AssetRegistry::ImportFile(const std::string& filename)
{
	const std::string assetFilePath = removeExtension(filename) + MetaFileExtension;
	std::filesystem::remove(assetFilePath);
	std::ofstream assetFile{ assetFilePath };

	::GUID guid;
	HRESULT hr = CoCreateGuid(&guid);

	char buffer[128];
	sprintf_s(buffer, "{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
		guid.Data1, guid.Data2, guid.Data3,
		guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
		guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);

	json newMeta = {
		{"guid", buffer}
	};

	assetFile << newMeta.dump();
	assetFile.close();
	
	return true;
}

EAssetType AssetRegistry::GetAssetTypeByExtension(const std::string& extension)
{
	if (extension == "shader")
	{
		return EAssetType::Shader;
	}
	return EAssetType::Data;
}

Asset* AssetRegistry::CreateAsset(EAssetType Type) const
{
	switch (Type)
	{
	case EAssetType::Shader:
		return new Asset();
	default:
		return new Asset();
	}
}

AssetRegistry::~AssetRegistry()
{
	for (auto& asset : assets)
	{
		delete asset.second;
	}
}