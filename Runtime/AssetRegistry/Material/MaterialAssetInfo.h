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

		virtual SAILOR_API void Serialize(nlohmann::json& outData) const override;
		virtual SAILOR_API void Deserialize(const nlohmann::json& inData) override;

	private:

	};

	using MaterialAssetInfoPtr = MaterialAssetInfo*;

	class MaterialAssetInfoHandler final : public TSubmodule<MaterialAssetInfoHandler>, public IAssetInfoHandler
	{

	public:

		SAILOR_API MaterialAssetInfoHandler(AssetRegistry* assetRegistry);

		virtual SAILOR_API void GetDefaultMetaJson(nlohmann::json& outDefaultJson) const;
		virtual SAILOR_API AssetInfoPtr CreateAssetInfo() const;

		virtual SAILOR_API ~MaterialAssetInfoHandler() = default;
	};
}
