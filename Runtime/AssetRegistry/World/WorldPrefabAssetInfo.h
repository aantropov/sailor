#pragma once
#include "AssetRegistry/AssetInfo.h"
#include "RHI/Types.h"
#include "Core/Singleton.hpp"

using namespace std;

namespace Sailor
{
	class WorldPrefabAssetInfo final : public AssetInfo
	{
		SAILOR_REFLECTABLE(WorldPrefabAssetInfo)

	public:
		SAILOR_API virtual ~WorldPrefabAssetInfo() = default;

		SAILOR_API virtual YAML::Node Serialize() const override;
		SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;

		SAILOR_API virtual IAssetInfoHandler* GetHandler() override;

	private:
	};

	using WorldPrefabAssetInfoPtr = WorldPrefabAssetInfo*;

	class SAILOR_API WorldPrefabAssetInfoHandler final : public TSubmodule<WorldPrefabAssetInfoHandler>, public IAssetInfoHandler
	{

	public:

		IAssetFactory* GetFactory() override;

		WorldPrefabAssetInfoHandler(AssetRegistry* assetRegistry);

		virtual void GetDefaultMeta(YAML::Node& outDefaultYaml) const override;
		AssetInfoPtr CreateAssetInfo() const override;

		virtual ~WorldPrefabAssetInfoHandler() = default;
	};
}

REFL_AUTO(
	type(Sailor::WorldPrefabAssetInfo, bases<Sailor::AssetInfo>),
	field(m_fileId),
	field(m_assetFilename)
)
