#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include "nlohmann_json/include/nlohmann/json.hpp"
#include "Singleton.hpp"

using namespace nlohmann;
namespace Sailor
{
	typedef std::string GUID;

	enum class EAssetType
	{
		Data = 0,
		Shader,
	};

	class Asset
	{
	public:

		GUID guid;
		std::string filePath;

		EAssetType assetType = EAssetType::Data;

		virtual void Serialize(json& outdata) const {}
		virtual void Deserialize(const json& data) {}

		virtual void Load() {}

		virtual ~Asset() = default;
	};

	class AssetRegistry : public Singleton<AssetRegistry>
	{
		const std::string ContentRootFolder = "..//Content//";
		const std::string MetaFileExtension = ".asset";

		std::unordered_map<GUID, Asset*> assets;

	public:

		virtual ~AssetRegistry() override;
		
		static void Initialize();
		static bool ReadFile(const std::string& filename, std::vector<char>& buffer);

		void LoadAll();
		void LoadAssetsInFolder(const std::string& folderPath);
		bool LoadAsset(const std::string& filename, bool bShouldImport = false);

		bool ImportFile(const std::string& filename);

		static EAssetType GetAssetTypeByExtension(const std::string& extension);

	protected:
		
		Asset* CreateAsset(EAssetType Type) const;
	};
}