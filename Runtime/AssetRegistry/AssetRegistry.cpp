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
	EASY_FUNCTION();

	m_pInstance = new AssetRegistry();

	DefaultAssetInfoHandler::Initialize();
	ShaderAssetInfoHandler::Initialize();

	m_pInstance->ScanContentFolder();
}

bool AssetRegistry::ReadAllTextFile(const std::string& filename, std::string& text)
{
	EASY_FUNCTION();

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
	EASY_FUNCTION();

	ScanFolder(ContentRootFolder);
}

bool AssetRegistry::RegisterAssetInfoHandler(const std::vector<std::string>& supportedExtensions, IAssetInfoHandler* assetInfoHandler)
{
	EASY_FUNCTION();

	bool bAssigned = false;
	for (const auto& extension : supportedExtensions)
	{
		if (m_assetInfoHandlers.find(extension) != m_assetInfoHandlers.end())
		{
			continue;
		}

		m_assetInfoHandlers[extension] = assetInfoHandler;
		bAssigned = true;
	}

	return bAssigned;
}

void AssetRegistry::ScanFolder(const std::string& folderPath)
{
	EASY_FUNCTION();

	for (const auto& entry : std::filesystem::directory_iterator(folderPath))
	{
		if (entry.is_directory())
		{
			ScanFolder(entry.path().string());
		}
		else if (entry.is_regular_file())
		{
			const std::string filepath = entry.path().string();
			const std::string extension = Utils::GetFileExtension(filepath);

			if (extension != MetaFileExtension)
			{
				const std::string assetInfoFile = Utils::RemoveFileExtension(filepath) + MetaFileExtension;

				IAssetInfoHandler* assetInfoHandler = DefaultAssetInfoHandler::GetInstance();

				auto assetInfoHandlerIt = m_assetInfoHandlers.find(extension);
				if (assetInfoHandlerIt != m_assetInfoHandlers.end())
				{
					assetInfoHandler = (*assetInfoHandlerIt).second;
				}

				auto uid = m_UIDs.find(filepath);
				if (uid != m_UIDs.end())
				{
					auto assetInfoIt = m_loadedAssetInfo.find(uid->second);
					if (assetInfoIt != m_loadedAssetInfo.end())
					{
						AssetInfo* assetInfo = assetInfoIt->second;
						if (assetInfo->IsExpired())
						{
							SAILOR_LOG("Reload asset info: %s", assetInfoFile.c_str());
							assetInfoHandler->ReloadAssetInfo(assetInfo);							
						}
						continue;
					}

					// Meta were delete
					m_UIDs.erase(uid);
				}

				AssetInfo* assetInfo = nullptr;
				if (std::filesystem::exists(assetInfoFile))
				{
					SAILOR_LOG("Load asset info: %s", assetInfoFile.c_str());
					assetInfo = assetInfoHandler->LoadAssetInfo(assetInfoFile);
				}
				else
				{
					SAILOR_LOG("Import new asset: %s", filepath.c_str());
					assetInfo = assetInfoHandler->ImportAsset(filepath);
				}

				m_loadedAssetInfo[assetInfo->GetUID()] = assetInfo;
				m_UIDs[filepath] = assetInfo->GetUID();

			}
		}
	}
}

AssetInfo* AssetRegistry::GetAssetInfo(const std::string& filepath) const
{
	auto it = m_UIDs.find(ContentRootFolder + filepath);
	if (it != m_UIDs.end())
	{
		return GetAssetInfo(it->second);
	}
	return nullptr;
}

AssetInfo* AssetRegistry::GetAssetInfo(UID uid) const
{
	EASY_FUNCTION();

	auto it = m_loadedAssetInfo.find(uid);
	if (it != m_loadedAssetInfo.end())
	{
		return it->second;
	}
	return nullptr;
}

AssetRegistry::~AssetRegistry()
{
	for (auto& asset : m_loadedAssetInfo)
	{
		delete asset.second;
	}

	DefaultAssetInfoHandler::Shutdown();
	ShaderAssetInfoHandler::Shutdown();
}