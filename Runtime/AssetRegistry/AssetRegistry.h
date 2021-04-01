#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include "UID.h"
#include "AssetInfo.h"
#include "Singleton.hpp"
#include "nlohmann_json/include/nlohmann/json.hpp"

using namespace nlohmann;

namespace Sailor
{
	class AssetInfo;
	enum class EAssetType;

	class AssetRegistry : public Singleton<AssetRegistry>
	{
		std::unordered_map<UID, AssetInfo*> loadedAssetInfos;
		std::unordered_map<std::string, class IAssetInfoHandler*> assetInfoHandlers;

	public:

		static constexpr const char* ContentRootFolder = "..//Content//";
		static constexpr const char* MetaFileExtension = "asset";

		virtual ~AssetRegistry() override;

		static void Initialize();
		static bool ReadFile(const std::string& filename, std::vector<char>& buffer);

		void ScanContentFolder();
		
		bool RegisterAssetInfoHandler(const std::vector<std::string>& supportedExtensions, IAssetInfoHandler* assetInfoHandler);

	protected:

		void ScanFolder(const std::string& folderPath);
	};
}
