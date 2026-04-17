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
		SAILOR_API virtual IAssetInfoHandler* GetHandler() override;
	private:

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
	field(m_assetFilename)
)
