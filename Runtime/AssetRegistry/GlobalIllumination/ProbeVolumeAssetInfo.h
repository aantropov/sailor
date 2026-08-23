#pragma once

#include "AssetRegistry/AssetInfo.h"
#include "Core/Singleton.hpp"

namespace Sailor
{
	class ProbeVolumeAssetInfo final : public AssetInfo
	{
		SAILOR_REFLECTABLE(ProbeVolumeAssetInfo)

	public:
		SAILOR_API ~ProbeVolumeAssetInfo() override = default;
		SAILOR_API YAML::Node Serialize() const override;
		SAILOR_API void Deserialize(const YAML::Node& inData) override;
		SAILOR_API IAssetInfoHandler* GetHandler() override;
	};

	using ProbeVolumeAssetInfoPtr = ProbeVolumeAssetInfo*;

	class ProbeVolumeAssetInfoHandler final :
		public TSubmodule<ProbeVolumeAssetInfoHandler>,
		public IAssetInfoHandler
	{
	public:
		SAILOR_API explicit ProbeVolumeAssetInfoHandler(AssetRegistry* assetRegistry);
		SAILOR_API void GetDefaultMeta(YAML::Node& outDefaultYaml) const override;
		SAILOR_API AssetInfoPtr CreateAssetInfo() const override;
		SAILOR_API IAssetFactory* GetFactory() override;
	};
}

REFL_AUTO(
	type(Sailor::ProbeVolumeAssetInfo, bases<Sailor::AssetInfo>),
	field(m_fileId),
	field(m_assetFilename)
)
