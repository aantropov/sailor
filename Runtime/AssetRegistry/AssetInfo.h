#pragma once
#include <string>
#include <ctime>
#include "AssetRegistry.h"
#include "UID.h"
#include "Singleton.hpp"
#include "JsonSerializable.h"
#include "nlohmann_json/include/nlohmann/json.hpp"

namespace Sailor
{
	class AssetInfo : IJsonSerializable
	{

	public:

		AssetInfo();
		virtual ~AssetInfo() = default;

		SAILOR_API const UID& GetUID() const { return m_UID; }
		SAILOR_API std::string GetAssetFilepath() const { return m_path + m_filename; }
		SAILOR_API std::time_t GetAssetLastModificationTime() const;

		virtual SAILOR_API void Serialize(nlohmann::json& outData) const override;
		virtual SAILOR_API void Deserialize(const nlohmann::json& inData) override;

	protected:

		std::time_t m_loadTime;
		std::string m_path;
		
		std::string m_filename;
		UID m_UID;

		friend class IAssetInfoHandler;
	};

	class IAssetInfoHandler
	{

	public:

		virtual SAILOR_API void GetDefaultAssetInfoMeta(nlohmann::json& outDefaultJson) const = 0;
		virtual SAILOR_API AssetInfo* CreateAssetInfo() const = 0;
		
		virtual SAILOR_API AssetInfo* LoadAssetInfo(const std::string& assetInfoPath) const;
		virtual SAILOR_API AssetInfo* ImportFile(const std::string& filepath) const;

		virtual SAILOR_API ~IAssetInfoHandler() = default;

	protected:

		std::vector<std::string> m_supportedExtensions;
	};

	class DefaultAssetInfoHandler final : public Singleton<DefaultAssetInfoHandler>, public IAssetInfoHandler
	{

	public:

		static SAILOR_API void Initialize();

		virtual SAILOR_API void GetDefaultAssetInfoMeta(nlohmann::json& outDefaultJson) const;
		virtual SAILOR_API AssetInfo* CreateAssetInfo() const;
		
		virtual SAILOR_API ~DefaultAssetInfoHandler() override = default;
	};
}
