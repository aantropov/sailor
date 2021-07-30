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

		template<typename T>
		static bool ReadBinaryFile(const std::string& filename, std::vector<T>& buffer)
		{
			std::ifstream file(filename, std::ios::ate | std::ios::binary);
			file.unsetf(std::ios::skipws);

			if (!file.is_open())
			{
				return false;
			}

			size_t fileSize = (size_t)file.tellg();
			buffer.clear();

			size_t mod = fileSize % sizeof(T);
			size_t size = fileSize / sizeof(T) + (mod ? 1 : 0);
			buffer.resize(size);
			
			//buffer.resize(fileSize / sizeof(T));

			file.seekg(0, std::ios::beg);
			file.read(reinterpret_cast<char*>(buffer.data()), fileSize);

			file.close();
			return true;
		}

		
		static SAILOR_API bool ReadAllTextFile(const std::string& filename, std::string& text);

		SAILOR_API void ScanContentFolder();
		
		SAILOR_API AssetInfo* GetAssetInfo(UID uid) const;
		SAILOR_API AssetInfo* GetAssetInfo(const std::string& assetFilepath) const;

		SAILOR_API bool RegisterAssetInfoHandler(const std::vector<std::string>& supportedExtensions, class IAssetInfoHandler* pAssetInfoHandler);

	protected:

		void ScanFolder(const std::string& folderPath);

		std::unordered_map<UID, AssetInfo*> m_loadedAssetInfo;
		std::unordered_map<std::string, UID> m_UIDs;
		std::unordered_map<std::string, class IAssetInfoHandler*> m_assetInfoHandlers;
	};
}
