#pragma once
#include <string>
#include <ctime>
#include "AssetRegistry.h"
#include "UID.h"
#include "Singleton.hpp"
#include "nlohmann_json/include/nlohmann/json.hpp"

using namespace nlohmann;
namespace Sailor { class AssetInfo; }

namespace ns
{
	SAILOR_API void to_json(json& j, const class Sailor::AssetInfo& p);
	SAILOR_API void from_json(const json& j, class Sailor::AssetInfo& p);
}

namespace Sailor
{
	class AssetInfo
	{

	public:

		AssetInfo();
		virtual ~AssetInfo() = default;

		SAILOR_API const UID& GetUID() const { return uid; }
		SAILOR_API const std::string& Getfilepath() const { return filepath; }

	protected:

		std::time_t creationTime;

		std::string assetType = "none";
		std::string filepath;

		UID uid;

		friend void ns::to_json(json& j, const AssetInfo& p);
		friend void ns::from_json(const json& j, AssetInfo& p);
		friend class IAssetInfoHandler;
	};

	class IAssetInfoHandler
	{

	public:

		virtual SAILOR_API void Serialize(const AssetInfo* inInfo, json& outData) const {}
		virtual SAILOR_API void Deserialize(const json& inData, AssetInfo* outInfo) const {}

		virtual SAILOR_API AssetInfo* ImportAssetInfo(const std::string& assetInfoPath) const { return nullptr; }
		virtual SAILOR_API AssetInfo* ImportFile(const std::string& filepath) const { return nullptr; }

		virtual SAILOR_API ~IAssetInfoHandler() = default;

	protected:

		std::vector<std::string> supportedExtensions;
	};

	
	class DefaultAssetInfoHandler final : public Singleton<DefaultAssetInfoHandler>, public IAssetInfoHandler
	{

	public:

		static SAILOR_API void Initialize();

		virtual SAILOR_API void Serialize(const AssetInfo* inInfo, json& outData) const override;
		virtual SAILOR_API void Deserialize(const json& inData, AssetInfo* outInfo) const override;

		virtual SAILOR_API AssetInfo* ImportAssetInfo(const std::string& assetInfoPath) const override;
		virtual SAILOR_API AssetInfo* ImportFile(const std::string& filepath) const override;

		virtual SAILOR_API ~DefaultAssetInfoHandler() = default;
	};
}
