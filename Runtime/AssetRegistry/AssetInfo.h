#pragma once
#include <array>
#include <string>
#include <string_view>
#include <ctime>
#include <type_traits>
#include "AssetRegistry/FileId.h"
#include "AssetRegistry/AssetMountDiscovery.h"
#include "Core/FileRevision.h"
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
		SAILOR_API bool IsWritable() const noexcept { return m_bWritable; }
		SAILOR_API EAssetMountKind GetMountKind() const noexcept { return m_mountKind; }
		SAILOR_API const std::string& GetVirtualAssetFilepath() const noexcept { return m_virtualAssetFilepath; }
		SAILOR_API const std::string& GetVirtualMetaFilepath() const noexcept { return m_virtualMetaFilepath; }

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

		SAILOR_API virtual bool SaveMetaFile();
		SAILOR_API virtual IAssetInfoHandler* GetHandler();

	protected:

		SAILOR_API virtual void CopyMetadata(const AssetInfo& source);

		std::time_t m_metaLoadTime;
		std::time_t m_assetImportTime;
		FileRevision m_importedSourceRevision;
		FileRevision m_metadataRevision;
		std::string m_folder;
		std::string m_assetFilename;
		std::string m_metaFilepath;
		std::string m_virtualAssetFilepath;
		std::string m_virtualMetaFilepath;
		FileId m_fileId;
		EAssetMountKind m_mountKind = EAssetMountKind::Workspace;
		bool m_bWritable = true;
		bool m_bPendingUpdateNotification = false;
		bool m_bPendingWasExpired = false;
		bool m_bPendingImportNotification = false;
		std::string m_importedMetadataContents;

		friend class IAssetInfoHandler;
		friend class AssetCache;
		friend class AssetRegistry;
	};

	using AssetInfoPtr = AssetInfo*;

	class SAILOR_API IAssetInfoHandlerListener
	{
	public:
		// Direct registration can make dependencies available to existing resources.
		// Most importers handle it like an update; dependent resources may need an owner task.
		virtual void OnRegisterAsset(AssetInfoPtr assetInfo, bool bWasExpired)
		{
			OnUpdateAssetInfo(assetInfo, bWasExpired);
		}

		// Listeners persist intentional metadata changes and acknowledge required
		// processing through AssetRegistry; notification alone does not save metadata.
		// bWasExpired means that the source or metadata changed since the last
		// acknowledged processing watermark, or that no watermark exists yet.
		// Explicit reimport also requests regeneration through this notification.
		// Importers refresh loaded resources here.
		virtual void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) = 0;
		// Called only after creating metadata for a previously untracked source.
		// A new source receives a non-expired registration/update notification first.
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

		virtual AssetInfoPtr LoadAssetInfo(
			const std::string& metaFilepath,
			const std::string& virtualMetaFilepath = {},
			EAssetMountKind mountKind = EAssetMountKind::Workspace,
			bool bWritable = true,
			bool bNotifyListeners = true,
			bool bUpdateAssetCache = true) const;
		virtual AssetInfoPtr ImportAsset(
			const std::string& assetFilepath,
			const std::string& virtualAssetFilepath = {},
			bool bNotifyListeners = true,
			bool bUpdateAssetCache = true) const;
		virtual bool ReloadAssetInfo(
			AssetInfoPtr assetInfo,
			bool bNotifyListeners = true,
			bool bUpdateAssetCache = true) const;
		bool DiscardImportedMetadataIfUnchanged(AssetInfoPtr assetInfo) const;
		void NotifyRegisterAsset(AssetInfoPtr assetInfo) const;
		void NotifyUpdateAssetInfo(AssetInfoPtr assetInfo, bool bReimport = false) const;
		void NotifyImportAsset(AssetInfoPtr assetInfo) const;

		virtual ~IAssetInfoHandler() = default;

		virtual IAssetFactory* GetFactory() { return nullptr; }

	protected:

		virtual AssetInfoPtr CreateAssetInfo() const = 0;

		TVector<IAssetInfoHandlerListener*> m_listeners;

	private:

		void NotifyAssetInfo(AssetInfoPtr assetInfo, bool bReimport, bool bRegistered) const;
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
	constexpr std::string_view NormalizeAssetInfoFieldName(std::string_view fieldName)
	{
		if (fieldName == "m_assetFilename")
		{
			return "filename";
		}

		if (fieldName.starts_with("m_"))
		{
			return fieldName.substr(2);
		}

		return fieldName;
	}

	namespace Attributes
	{
		template<typename... TExtensions>
		struct Asset : refl::attr::usage::type
		{
			constexpr Asset(TExtensions... extensions) : m_extensions{ extensions... } {}

			TVector<std::string> Extensions() const
			{
				TVector<std::string> extensions;
				for (const auto extension : m_extensions)
				{
					extensions.Emplace(extension);
				}
				return extensions;
			}

			template<typename TAssetInfo>
			YAML::Node Serialize() const
			{
				static_assert(std::is_base_of_v<AssetInfo, TAssetInfo>);
				YAML::Node node;
				node["typename"] = refl::reflect<TAssetInfo>().name.c_str();
				node["extensions"] = YAML::Node(YAML::NodeType::Sequence);
				for (const auto extension : m_extensions)
				{
					node["extensions"].push_back(extension);
				}

				YAML::Node properties(YAML::NodeType::Sequence);
				TVector<std::string_view> names;
				TAssetInfo* empty = nullptr;
				for_each(refl::reflect<TAssetInfo>().members, [&](auto member)
					{
						if constexpr (is_writable(member))
						{
							const std::string_view name = NormalizeAssetInfoFieldName(get_display_name(member));
							if (names.Contains(name))
							{
								return;
							}
							names.Add(name);
							using PropertyType = decltype(get_reader(member)(*empty));
							YAML::Node property;
							property["name"] = name;
							property["type"] = TypeInfo::GetReflectedPropertyTypeName<PropertyType>();
							properties.push_back(property);
						}
					});
				node["properties"] = properties;
				return node;
			}

		private:
			std::array<std::string_view, sizeof...(TExtensions)> m_extensions;
		};
	}

	template<typename TAssetInfo>
	TVector<std::string> GetAssetInfoExtensions()
	{
		return refl::descriptor::get_attribute<Attributes::Asset>(refl::reflect<TAssetInfo>()).Extensions();
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
					const std::string_view displayName = NormalizeAssetInfoFieldName(get_display_name(member));
					outData[displayName] = member(assetInfo);
				}
			});

		return outData;
	}

	template<typename TAssetInfo>
	YAML::Node CreateAssetInfoMetadata(const FileId& fileId, std::string_view filename)
	{
		static_assert(std::is_base_of_v<AssetInfo, TAssetInfo>);

		TAssetInfo defaultObject;
		YAML::Node outData = defaultObject.Serialize();
		outData["assetInfoType"] = defaultObject.GetAssetInfoType();
		outData["fileId"] = fileId.Serialize();
		outData["filename"] = filename;
		return outData;
	}

	template<typename TAssetInfo>
	void CopyReflectedAssetInfo(TAssetInfo& destination, const TAssetInfo& source)
	{
		for_each(refl::reflect<TAssetInfo>().members, [&](auto member)
			{
				if constexpr (is_readable(member) && is_writable(member))
				{
					if constexpr (is_field(member))
					{
						member(destination) = member(source);
					}
					else if constexpr (refl::descriptor::is_function(member))
					{
						member(destination, get_reader(member)(source));
					}
				}
			});
	}

	template<typename TAssetInfo>
	void DeserializeReflectedAssetInfo(TAssetInfo& assetInfo, const YAML::Node& inData)
	{
		for_each(refl::reflect(assetInfo).members, [&](auto member)
			{
				if constexpr (is_writable(member))
				{
					const std::string_view displayName = NormalizeAssetInfoFieldName(get_display_name(member));
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
	type(Sailor::AssetInfo, Sailor::Attributes::Asset{}),
	field(m_fileId),
	field(m_assetFilename)
)
