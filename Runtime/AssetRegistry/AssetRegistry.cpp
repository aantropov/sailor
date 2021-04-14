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

bool AssetRegistry::ReadFile(const std::string& filename, std::string& text)
{
	constexpr auto readSize = std::size_t{ 4096 };
	auto stream = std::ifstream{ filename.data() };
	stream.exceptions(std::ios_base::badbit);

	if (!stream.is_open())
	{
		return false;
	}

	text = std::string{};
	auto buf = std::string(readSize, '\0');
	while (stream.read(&buf[0], readSize)) 
	{
		text.append(buf, 0, stream.gcount());
	}
	text.append(buf, 0, stream.gcount());

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
			const std::string filepath = entry.path().string();
			const std::string extension = Utils::GetExtension(filepath);

			if (extension != MetaFileExtension)
			{
				const std::string assetInfoFile = Utils::RemoveExtension(filepath) + MetaFileExtension;

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
					assetInfo = assetInfoHandler->ImportFile(filepath);
				}

				loadedAssetInfos[assetInfo->GetUID()] = assetInfo;
				uids[filepath] = assetInfo->GetUID();
			}
		}
	}
}

AssetInfo* AssetRegistry::GetAssetInfo(const std::string& filepath) const
{
	auto it = uids.find(ContentRootFolder + filepath);
	if (it != uids.end())
	{
		return GetAssetInfo(it->second);
	}
	return nullptr;

}

AssetInfo* AssetRegistry::GetAssetInfo(UID uid) const
{
	auto it = loadedAssetInfos.find(uid);
	if (it != loadedAssetInfos.end())
	{
		return it->second;
	}
	return nullptr;
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