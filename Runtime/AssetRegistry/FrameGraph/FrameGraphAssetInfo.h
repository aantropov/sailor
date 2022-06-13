#pragma once
#include "AssetRegistry/AssetInfo.h"
#include "RHI/Types.h"
#include "Core/Singleton.hpp"

using namespace std;

namespace Sailor
{
	class FrameGraphAssetInfo final : public AssetInfo
	{
	public:
		SAILOR_API virtual ~FrameGraphAssetInfo() = default;

		SAILOR_API virtual void Serialize(nlohmann::json& outData) const override;
		SAILOR_API virtual void Deserialize(const nlohmann::json& inData) override;
	};

	using FrameGraphAssetInfoPtr = FrameGraphAssetInfo*;

	class SAILOR_API FrameGraphAssetInfoHandler final : public TSubmodule<FrameGraphAssetInfoHandler>, public IAssetInfoHandler
	{
	public:

		FrameGraphAssetInfoHandler(AssetRegistry* assetRegistry);

		virtual void GetDefaultMetaJson(nlohmann::json& outDefaultJson) const;
		virtual AssetInfoPtr CreateAssetInfo() const;

		virtual ~FrameGraphAssetInfoHandler() = default;
	};
}
