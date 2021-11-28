#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include "AssetRegistry/UID.h"
#include "Core/Submodule.h"
#include "AssetRegistry/AssetInfo.h"
#include "Core/Singleton.hpp"
#include "nlohmann_json/include/nlohmann/json.hpp"

namespace Sailor
{
	class AssetInfo;
	using AssetInfoPtr = AssetInfo*;

	enum class EAssetType;

	class AssetRegistry final : public TSubmodule<AssetRegistry>
	{
	public:

		static constexpr const char* ContentRootFolder = "../Content/";
		static constexpr const char* MetaFileExtension = "asset";

		virtual SAILOR_API ~AssetRegistry() override;

		template<typename TBinaryType, typename TFilepath>
		static bool ReadBinaryFile(const TFilepath& filename, std::vector<TBinaryType>& buffer)
		{
			std::ifstream file(filename, std::ios::ate | std::ios::binary);
			file.unsetf(std::ios::skipws);

			if (!file.is_open())
			{
				return false;
			}

			size_t fileSize = (size_t)file.tellg();
			buffer.clear();

			size_t mod = fileSize % sizeof(TBinaryType);
			size_t size = fileSize / sizeof(TBinaryType) + (mod ? 1 : 0);
			buffer.resize(size);

			//buffer.resize(fileSize / sizeof(T));

			file.seekg(0, std::ios::beg);
			file.read(reinterpret_cast<char*>(buffer.data()), fileSize);

			file.close();
			return true;
		}

		static SAILOR_API bool ReadAllTextFile(const std::string& filename, std::string& text);

		SAILOR_API void ScanContentFolder();
		SAILOR_API void ScanFolder(const std::string& folderPath);
		SAILOR_API const UID& LoadAsset(const std::string& filepath);
		SAILOR_API const UID& GetOrLoadAsset(const std::string& filepath);

		template<typename TAssetInfoPtr = AssetInfoPtr>
		TAssetInfoPtr GetAssetInfoPtr(UID uid) const
		{
			return 	dynamic_cast<TAssetInfoPtr>(GetAssetInfoPtr_Internal(uid));
		}

		template<typename TAssetInfoPtr = AssetInfoPtr>
		TAssetInfoPtr GetAssetInfoPtr(const std::string& assetFilepath) const
		{
			return 	dynamic_cast<TAssetInfoPtr>(GetAssetInfoPtr_Internal(assetFilepath));
		}

		template<class TAssetInfo>
		void GetAllAssetInfos(std::vector<UID>& outAssetInfos) const
		{
			outAssetInfos.clear();
			for (const auto& assetInfo : m_loadedAssetInfo)
			{
				if (dynamic_cast<TAssetInfo*>(assetInfo.second))
				{
					outAssetInfos.push_back(assetInfo.first);
				}
			}
		}

		SAILOR_API bool RegisterAssetInfoHandler(const std::vector<std::string>& supportedExtensions, class IAssetInfoHandler* pAssetInfoHandler);
		static SAILOR_API std::string GetMetaFilePath(const std::string& assetFilePath);

	protected:

		SAILOR_API AssetInfoPtr GetAssetInfoPtr_Internal(UID uid) const;
		SAILOR_API AssetInfoPtr GetAssetInfoPtr_Internal(const std::string& assetFilepath) const;

		std::unordered_map<UID, AssetInfoPtr> m_loadedAssetInfo;
		std::unordered_map<std::string, UID> m_UIDs;
		std::unordered_map<std::string, class IAssetInfoHandler*> m_assetInfoHandlers;
	};
}
