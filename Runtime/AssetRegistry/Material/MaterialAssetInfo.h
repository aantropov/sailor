#pragma once
#include "AssetRegistry/AssetInfo.h"
#include "RHI/Types.h"
#include "Core/Singleton.hpp"

using namespace std;

namespace Sailor
{
	class MaterialAssetInfo final : public AssetInfo
	{
		SAILOR_REFLECTABLE(MaterialAssetInfo)

	public:
		virtual SAILOR_API ~MaterialAssetInfo() = default;
		SAILOR_API YAML::Node Serialize() const override;
		SAILOR_API void Deserialize(const YAML::Node& inData) override;
		SAILOR_API const FileId& GetSourceModel() const { return m_sourceModel; }
		SAILOR_API int32_t GetSourceMaterialIndex() const { return m_sourceMaterialIndex; }
		SAILOR_API virtual IAssetInfoHandler* GetHandler() override;
	private:
		SAILOR_API void CopyMetadata(const AssetInfo& source) override;
		FileId m_sourceModel;
		int32_t m_sourceMaterialIndex = -1;
	};

	using MaterialAssetInfoPtr = MaterialAssetInfo*;

	class MaterialAssetInfoHandler final : public TSubmodule<MaterialAssetInfoHandler>, public IAssetInfoHandler
	{

	public:

		IAssetFactory* GetFactory() override;

		SAILOR_API MaterialAssetInfoHandler(AssetRegistry* assetRegistry);

		SAILOR_API virtual void GetDefaultMeta(YAML::Node& outDefaultYaml) const override;
		SAILOR_API AssetInfoPtr CreateAssetInfo() const override;

		SAILOR_API virtual ~MaterialAssetInfoHandler() = default;
	};
}

REFL_AUTO(
	type(Sailor::MaterialAssetInfo, bases<Sailor::AssetInfo>),
	field(m_fileId),
	field(m_assetFilename),
	field(m_sourceModel),
	field(m_sourceMaterialIndex)
)
