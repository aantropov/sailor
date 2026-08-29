#pragma once

#include "AssetRegistry/AssetInfo.h"
#include "Core/Singleton.hpp"

namespace Sailor
{
	class GIProbesAssetInfo final : public AssetInfo
	{
		SAILOR_REFLECTABLE(GIProbesAssetInfo)

	public:
		SAILOR_API ~GIProbesAssetInfo() override = default;
		SAILOR_API YAML::Node Serialize() const override;
		SAILOR_API void Deserialize(const YAML::Node& inData) override;
		SAILOR_API IAssetInfoHandler* GetHandler() override;
	};

	using GIProbesAssetInfoPtr = GIProbesAssetInfo*;

	class GIProbesAssetInfoHandler final :
		public TSubmodule<GIProbesAssetInfoHandler>,
		public IAssetInfoHandler
	{
	public:
		SAILOR_API explicit GIProbesAssetInfoHandler(AssetRegistry* assetRegistry);
		SAILOR_API void GetDefaultMeta(YAML::Node& outDefaultYaml) const override;
		SAILOR_API AssetInfoPtr CreateAssetInfo() const override;
		SAILOR_API IAssetFactory* GetFactory() override;
	};
}

REFL_AUTO(
	type(Sailor::GIProbesAssetInfo, bases<Sailor::AssetInfo>),
	field(m_fileId),
	field(m_assetFilename)
)
