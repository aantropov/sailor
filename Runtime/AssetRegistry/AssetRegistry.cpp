#include "AssetRegistry.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include "AssetInfo.h"
#include "ShaderAssetInfo.h"
#include "Utils.h"
#include "nlohmann_json/include/nlohmann/json.hpp"

using namespace Sailor;
using namespace nlohmann;

void AssetRegistry::Initialize()
{
	instance = new AssetRegistry();

	DefaultAssetInfoHandler::Initialize();
	ShaderAssetInfoHandler::Initialize();

	instance->ScanContentFolder();
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
			const std::string file = entry.path().string();
			const std::string extension = Utils::GetExtension(file);

			if (extension != MetaFileExtension)
			{
				const std::string assetInfoFile = Utils::RemoveExtension(file) + MetaFileExtension;

				IAssetInfoHandler* assetInfoHandler = DefaultAssetInfoHandler::GetInstance();

				auto assetInfoHandlerIt = assetInfoHandlers.find(extension);
				if (assetInfoHandlerIt != assetInfoHandlers.end())
				{
					assetInfoHandler = (*assetInfoHandlerIt).second;
				}

				AssetInfo* assetInfo = nullptr;
				if (std::filesystem::exists(assetInfoFile))
				{
					assetInfo = assetInfoHandler->ImportAssetInfo(assetInfoFile);
				}
				else
				{
					assetInfo = assetInfoHandler->ImportFile(file);
				}

				loadedAssetInfos[assetInfo->GetUID()] = assetInfo;
			}
		}
	}
}

AssetRegistry::~AssetRegistry()
{
	for (auto& asset : loadedAssetInfos)
	{
		delete asset.second;
	}

	DefaultAssetInfoHandler::Shutdown();
	ShaderAssetInfoHandler::Shutdown();
}