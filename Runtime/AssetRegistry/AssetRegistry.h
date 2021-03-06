#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include "UID.h"
#include "Sailor.h"
#include "AssetInfo.h"
#include "Singleton.hpp"
#include "nlohmann_json/include/nlohmann/json.hpp"

namespace Sailor
{
	class AssetInfo;
	enum class EAssetType;

	class AssetRegistry final : public Singleton<AssetRegistry>
	{
	public:

		static constexpr const char* ContentRootFolder = "..//Content//";
		static constexpr const char* MetaFileExtension = "asset";

		virtual SAILOR_API ~AssetRegistry() override;

		static SAILOR_API void Initialize();
		static SAILOR_API bool ReadFile(const std::string& filename, std::vector<char>& buffer);
		static SAILOR_API bool ReadFile(const std::string& filename, std::string& text);

		SAILOR_API void ScanContentFolder();
		
		SAILOR_API AssetInfo* GetAssetInfo(UID uid) const;
		SAILOR_API AssetInfo* GetAssetInfo(const std::string& filepath) const;

		SAILOR_API bool RegisterAssetInfoHandler(const std::vector<std::string>& supportedExtensions, class IAssetInfoHandler* pAssetInfoHandler);

	protected:

		void ScanFolder(const std::string& folderPath);

		std::unordered_map<UID, AssetInfo*> m_loadedAssetInfos;
		std::unordered_map<std::string, UID> m_UIDs;
		std::unordered_map<std::string, class IAssetInfoHandler*> m_assetInfoHandlers;
	};
}
