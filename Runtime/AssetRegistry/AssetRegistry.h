#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "AssetInfo.h"
#include "Singleton.hpp"

namespace Sailor
{
	typedef std::string GUID;
	
	class AssetInfo;
	enum class EAssetType;
	
	class AssetRegistry : public Singleton<AssetRegistry>
	{
		const std::string ContentRootFolder = "..//Content//";
		const std::string MetaFileExtension = ".asset";

		std::unordered_map<GUID, AssetInfo*> loadedAssets;
		std::vector<std::string> filesToImport;

		std::unordered_map<const std::string, IAssetInfoHandler*> assetInfoHandlers;
		
	public:

		virtual ~AssetRegistry() override;
		
		static void Initialize();
		static bool ReadFile(const std::string& filename, std::vector<char>& buffer);

		void ScanContentFolder();

		void LoadAll();
		void LoadAssetsInFolder(const std::string& folderPath);
		bool LoadAsset(const std::string& filename, bool bShouldImport = false);

		bool ImportFile(const std::string& filename);

		static EAssetType GetAssetTypeByExtension(const std::string& extension);

		bool RegisterAssetInfoHandler(const std::vector<std::string>& supportedExtensions, IAssetInfoHandler* assetInfoHandler);
		
	protected:

		void ScanFolder(const std::string& folderPath);
		
		AssetInfo* CreateAsset(EAssetType Type) const;
	};
}