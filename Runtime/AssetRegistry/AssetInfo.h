#pragma once
#include <string>
#include "AssetRegistry.h"
#include "UID.h"
#include "Singleton.hpp"
#include "nlohmann_json/include/nlohmann/json.hpp"

using namespace nlohmann;
namespace Sailor { class AssetInfo; }

namespace ns
{
	void to_json(json& j, const class Sailor::AssetInfo& p);
	void from_json(const json& j, class Sailor::AssetInfo& p);
}

namespace Sailor
{
	class AssetInfo
	{

	public:

		virtual ~AssetInfo() = default;

		const UID& GetUID() const { return uid; }
		const std::string& GetFilePath() const { return filePath; }

	protected:

		std::string assetType = "none";
		std::string filePath;

		UID uid;

		friend void ns::to_json(json& j, const AssetInfo& p);
		friend void ns::from_json(const json& j, AssetInfo& p);
		friend class IAssetInfoHandler;
	};

	class IAssetInfoHandler
	{

	public:

		virtual void Serialize(const AssetInfo* inInfo, json& outData) const {}
		virtual void Deserialize(const json& inData, AssetInfo* outInfo) const {}

		virtual AssetInfo* ImportAssetInfo(const std::string& assetInfoPath) const { return nullptr; }
		virtual AssetInfo* ImportFile(const std::string& filePath) const { return nullptr; }

		virtual ~IAssetInfoHandler() = default;

	protected:

		std::vector<std::string> supportedExtensions;
	};

	
	class DefaultAssetInfoHandler : public Singleton<DefaultAssetInfoHandler>, public IAssetInfoHandler
	{

	public:

		static void Initialize();

		virtual void Serialize(const AssetInfo* inInfo, json& outData) const override;
		virtual void Deserialize(const json& inData, AssetInfo* outInfo) const override;

		virtual AssetInfo* ImportAssetInfo(const std::string& assetInfoPath) const override;
		virtual AssetInfo* ImportFile(const std::string& filePath) const override;

		virtual ~DefaultAssetInfoHandler() = default;
	};
}
