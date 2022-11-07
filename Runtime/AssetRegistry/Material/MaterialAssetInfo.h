#pragma once
#include "AssetRegistry/AssetInfo.h"
#include "RHI/Types.h"
#include "Core/Singleton.hpp"

using namespace std;

namespace Sailor
{
	class MaterialAssetInfo final : public AssetInfo
	{
	public:
		virtual SAILOR_API ~MaterialAssetInfo() = default;

	private:

	};

	using MaterialAssetInfoPtr = MaterialAssetInfo*;

	class MaterialAssetInfoHandler final : public TSubmodule<MaterialAssetInfoHandler>, public IAssetInfoHandler
	{

	public:

		SAILOR_API MaterialAssetInfoHandler(AssetRegistry* assetRegistry);

		SAILOR_API virtual void GetDefaultMeta(YAML::Node& outDefaultYaml) const;
		SAILOR_API virtual AssetInfoPtr CreateAssetInfo() const;

		SAILOR_API virtual ~MaterialAssetInfoHandler() = default;
	};
}
