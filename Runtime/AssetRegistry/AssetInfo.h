#pragma once
#include <string>
#include <ctime>
#include "AssetRegistry/FileId.h"
#include "Core/Singleton.hpp"
#include "Containers/Vector.h"
#include "Core/YamlSerializable.h"
#include "Core/Reflection.h"

namespace Sailor
{
	class IAssetFactory;
	class IAssetInfoHandler;

	class AssetInfo : public IYamlSerializable, public IReflectable
	{
		SAILOR_REFLECTABLE(AssetInfo)

	public:

		SAILOR_API AssetInfo();
		SAILOR_API virtual ~AssetInfo() = default;

		SAILOR_API std::string GetAssetInfoType() const;

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

namespace Sailor
{
	inline std::string NormalizeAssetInfoFieldName(const std::string& fieldName)
	{
		if (fieldName == "m_assetFilename")
		{
			return "filename";
		}

		if (fieldName.rfind("m_", 0) == 0)
		{
			return fieldName.substr(2);
		}

		return fieldName;
	}

	template<typename TAssetInfo>
	YAML::Node SerializeReflectedAssetInfo(const TAssetInfo& assetInfo)
	{
		YAML::Node outData;
		outData["assetInfoType"] = assetInfo.GetAssetInfoType();

		for_each(refl::reflect(assetInfo).members, [&](auto member)
			{
				if constexpr (is_readable(member))
				{
					const std::string displayName = NormalizeAssetInfoFieldName(get_display_name(member));
					outData[displayName] = member(assetInfo);
				}
			});

		return outData;
	}

	template<typename TAssetInfo>
	void DeserializeReflectedAssetInfo(TAssetInfo& assetInfo, const YAML::Node& inData)
	{
		for_each(refl::reflect(assetInfo).members, [&](auto member)
			{
				if constexpr (is_writable(member))
				{
					const std::string displayName = NormalizeAssetInfoFieldName(get_display_name(member));
					const YAML::Node& node = inData[displayName];
					if (node.IsDefined())
					{
						if constexpr (is_field(member))
						{
							using PropertyType = ::refl::trait::remove_qualifiers_t<decltype(member(assetInfo))>;
							member(assetInfo) = node.as<PropertyType>();
						}
						else if constexpr (refl::descriptor::is_function(member))
						{
							using PropertyType = ::refl::trait::remove_qualifiers_t<decltype(get_reader(member)(assetInfo))>;
							member(assetInfo, node.as<PropertyType>());
						}
					}
				}
			});
	}
}

REFL_AUTO(
	type(Sailor::AssetInfo),
	field(m_fileId),
	field(m_assetFilename)
)
