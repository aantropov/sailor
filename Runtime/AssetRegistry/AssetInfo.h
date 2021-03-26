#pragma once
#include <string>
#include "AssetRegistry.h"
#include "nlohmann_json/include/nlohmann/json.hpp"

using namespace nlohmann;

namespace ns
{
	void to_json(json& j, const Sailor::AssetInfo& p);
	void from_json(const json& j, Sailor::AssetInfo& p);
}

namespace Sailor
{
	class AssetInfo
	{

	public:

		virtual ~AssetInfo() = default;
		const GUID& GetGUID() const { return guid; }

	protected:

		std::string assetType = "none";
		GUID guid;

		friend void ns::to_json(json& j, const Sailor::AssetInfo& p);
		friend void ns::from_json(const json& j, Sailor::AssetInfo& p);
	};

	class IAssetInfoHandler
	{

	public:

		virtual void Serialize(const AssetInfo* inInfo, json& outData) const = 0;
		virtual void Deserialize(const json& inData, AssetInfo* outInfo) const = 0;

		virtual AssetInfo* ImportFile(const std::string& filePath) const = 0;

		virtual ~IAssetInfoHandler() = default;

	protected:

		std::vector<std::string> allowedExtensions;
	};

	class AssetInfoHandler : public Singleton<AssetInfoHandler>, public IAssetInfoHandler
	{
	public:

		static void Initialize();

		virtual void Serialize(const AssetInfo* inInfo, json& outData) const override;
		virtual void Deserialize(const json& inData, AssetInfo* outInfo) const override;

		virtual AssetInfo* ImportFile(const std::string& filePath) const override;

		virtual ~AssetInfoHandler() = default;
	};
}
