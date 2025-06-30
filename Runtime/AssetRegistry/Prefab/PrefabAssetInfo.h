#pragma once
#include "AssetRegistry/AssetInfo.h"
#include "RHI/Types.h"
#include "Core/Singleton.hpp"

using namespace std;

namespace Sailor
{
	class PrefabAssetInfo final : public AssetInfo
	{
	public:
		SAILOR_API virtual ~PrefabAssetInfo() = default;

		SAILOR_API virtual YAML::Node Serialize() const override;
		SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;
		SAILOR_API virtual IAssetInfoHandler* GetHandler() override;

	private:
	};

	using PrefabAssetInfoPtr = PrefabAssetInfo*;

	class SAILOR_API PrefabAssetInfoHandler final : public TSubmodule<PrefabAssetInfoHandler>, public IAssetInfoHandler
	{

	public:

		IAssetFactory* GetFactory() override;

		PrefabAssetInfoHandler(AssetRegistry* assetRegistry);

		virtual void GetDefaultMeta(YAML::Node& outDefaultYaml) const override;
		AssetInfoPtr CreateAssetInfo() const override;

		virtual ~PrefabAssetInfoHandler() = default;
	};
}
