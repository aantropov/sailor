#pragma once
#include "AssetRegistry/AssetInfo.h"
#include "Core/Singleton.hpp"

using namespace std;

namespace Sailor
{
	class ShaderAssetInfo final : public AssetInfo
	{
	public:
		SAILOR_API virtual ~ShaderAssetInfo() = default;
	};

	using ShaderAssetInfoPtr = ShaderAssetInfo*;

	class ShaderAssetInfoHandler final : public TSubmodule<ShaderAssetInfoHandler>, public IAssetInfoHandler
	{

	public:

		SAILOR_API ShaderAssetInfoHandler(AssetRegistry* assetRegistry);

		SAILOR_API virtual void GetDefaultMetaJson(nlohmann::json& outDefaultJson) const;
		SAILOR_API virtual AssetInfoPtr CreateAssetInfo() const;

		SAILOR_API virtual ~ShaderAssetInfoHandler() = default;
	};
}
