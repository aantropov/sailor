#pragma once
#include <string>
#include <ctime>
#include "AssetRegistry/FileId.h"
#include "Core/Singleton.hpp"
#include "Containers/Vector.h"
#include "Core/YamlSerializable.h"

namespace Sailor
{
	class IAssetFactory;
	class IAssetInfoHandler;

	class AssetInfo : IYamlSerializable
	{

	public:

		SAILOR_API AssetInfo();
		SAILOR_API virtual ~AssetInfo() = default;

		SAILOR_API const FileId& GetFileId() const { return m_fileId; }

		SAILOR_API std::string GetRelativeAssetFilepath() const;
		SAILOR_API std::string GetRelativeMetaFilepath() const;

		// That includes "../Content/" in the beginning
		SAILOR_API std::string GetAssetFilepath() const { return m_folder + m_assetFilename; }

		SAILOR_API const std::string& GetAssetFilename() const { return m_assetFilename; }

		// That includes "../Content/" in the beginning
		SAILOR_API std::string GetMetaFilepath() const;

		SAILOR_API std::time_t GetAssetImportTime() const;
		SAILOR_API std::time_t GetAssetLastModificationTime() const;
		SAILOR_API std::time_t GetMetaLastModificationTime() const;

		SAILOR_API bool IsMetaExpired() const;
		SAILOR_API bool IsAssetExpired() const;

		SAILOR_API virtual YAML::Node Serialize() const override;
		SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;

		SAILOR_API virtual void SaveMetaFile();
		SAILOR_API virtual IAssetInfoHandler* GetHandler();

	protected:

		std::time_t m_metaLoadTime;
		std::time_t m_assetImportTime;
		std::string m_folder;
		std::string m_assetFilename;
		FileId m_fileId;

		friend class IAssetInfoHandler;
	};

	using AssetInfoPtr = AssetInfo*;

	class SAILOR_API IAssetInfoHandlerListener
	{
	public:
		virtual void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) = 0;
		virtual void OnImportAsset(AssetInfoPtr assetInfo) = 0;

	};

	class SAILOR_API IAssetInfoHandler
	{

	public:

		void Subscribe(IAssetInfoHandlerListener* listener) { m_listeners.Add(listener); }
		void Unsubscribe(IAssetInfoHandlerListener* listener)
		{
			m_listeners.Remove(listener);
		}

		virtual void GetDefaultMeta(YAML::Node& outDefaultYaml) const = 0;

		virtual AssetInfoPtr LoadAssetInfo(const std::string& metaFilepath) const;
		virtual AssetInfoPtr ImportAsset(const std::string& assetFilepath) const;
		virtual void ReloadAssetInfo(AssetInfoPtr assetInfo) const;

		virtual ~IAssetInfoHandler() = default;

		virtual IAssetFactory* GetFactory() { return nullptr; }

	protected:

		virtual AssetInfoPtr CreateAssetInfo() const = 0;
		TVector<std::string> m_supportedExtensions;

		TVector<IAssetInfoHandlerListener*> m_listeners;
	};

	class SAILOR_API DefaultAssetInfoHandler final : public TSubmodule<DefaultAssetInfoHandler>, public IAssetInfoHandler
	{

	public:

		DefaultAssetInfoHandler(class AssetRegistry* assetRegistry);

		virtual void GetDefaultMeta(YAML::Node& outDefaultYaml) const override;
		virtual AssetInfoPtr CreateAssetInfo() const override;

		virtual ~DefaultAssetInfoHandler() override = default;
	};
}
