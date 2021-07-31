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
		SAILOR_API std::string GetAssetFilepath() const { return m_folder + m_assetFilename; }
		SAILOR_API std::string GetMetaFilepath() const;

		SAILOR_API std::time_t GetAssetLastModificationTime() const;
		SAILOR_API std::time_t GetMetaLastModificationTime() const;

		SAILOR_API bool IsExpired() const;

		virtual SAILOR_API void Serialize(nlohmann::json& outData) const override;
		virtual SAILOR_API void Deserialize(const nlohmann::json& inData) override;

	protected:

		std::time_t m_loadTime;
		std::string m_folder;
		std::string m_assetFilename;
		UID m_UID;

		friend class IAssetInfoHandler;
	};

	class IAssetInfoHandlerListener
	{
	public:
		virtual SAILOR_API void OnAssetInfoUpdated(AssetInfo* assetInfo) {}
	};

	class IAssetInfoHandler
	{

	public:

		SAILOR_API void Subscribe(IAssetInfoHandlerListener* listener) { m_listeners.push_back(listener); }
		SAILOR_API void Unsubscribe(IAssetInfoHandlerListener* listener)
		{
			m_listeners.erase(std::find(m_listeners.begin(), m_listeners.end(), listener));
		}

		virtual SAILOR_API void GetDefaultMetaJson(nlohmann::json& outDefaultJson) const = 0;

		virtual SAILOR_API AssetInfo* LoadAssetInfo(const std::string& metaFilepath) const;
		virtual SAILOR_API AssetInfo* ImportAsset(const std::string& assetFilepath) const;
		virtual SAILOR_API void ReloadAssetInfo(AssetInfo* assetInfo) const;

		virtual SAILOR_API ~IAssetInfoHandler() = default;

	protected:

		virtual SAILOR_API AssetInfo* CreateAssetInfo() const = 0;
		std::vector<std::string> m_supportedExtensions;

		std::vector<IAssetInfoHandlerListener*> m_listeners;
	};

	class DefaultAssetInfoHandler final : public Singleton<DefaultAssetInfoHandler>, public IAssetInfoHandler
	{

	public:

		static SAILOR_API void Initialize();

		virtual SAILOR_API void GetDefaultMetaJson(nlohmann::json& outDefaultJson) const;
		virtual SAILOR_API AssetInfo* CreateAssetInfo() const;

		virtual SAILOR_API ~DefaultAssetInfoHandler() override = default;
	};
}
