#pragma once
#include <string>
#include <ctime>
#include "AssetRegistry.h"
#include "UID.h"
#include "Singleton.hpp"
#include "JsonSerializable.h"
#include "nlohmann_json/include/nlohmann/json.hpp"

using namespace nlohmann;
namespace Sailor { class AssetInfo; }

namespace Sailor
{
	class AssetInfo : IJsonSerializable
	{

	public:

		AssetInfo();
		virtual ~AssetInfo() = default;

		SAILOR_API const UID& GetUID() const { return m_UID; }
		SAILOR_API const std::string& GetAssetFilepath() const { return m_filepath; }

		virtual SAILOR_API void Serialize(nlohmann::json& outData) const override;
		virtual SAILOR_API void Deserialize(const nlohmann::json& inData) override;
		
	protected:

		std::time_t m_creationTime;

		std::string m_assetType = "none";
		std::string m_filepath;

		UID m_UID;

		friend class IAssetInfoHandler;
	};

	class IAssetInfoHandler
	{

	public:

		virtual SAILOR_API AssetInfo* ImportAssetInfo(const std::string& assetInfoPath) const { return nullptr; }
		virtual SAILOR_API AssetInfo* ImportFile(const std::string& filepath) const { return nullptr; }

		virtual SAILOR_API ~IAssetInfoHandler() = default;

	protected:

		std::vector<std::string> m_supportedExtensions;
	};

	
	class DefaultAssetInfoHandler final : public Singleton<DefaultAssetInfoHandler>, public IAssetInfoHandler
	{

	public:

		static SAILOR_API void Initialize();

		virtual SAILOR_API AssetInfo* ImportAssetInfo(const std::string& assetInfoPath) const override;
		virtual SAILOR_API AssetInfo* ImportFile(const std::string& filepath) const override;

		virtual SAILOR_API ~DefaultAssetInfoHandler() override = default;
	};
}
