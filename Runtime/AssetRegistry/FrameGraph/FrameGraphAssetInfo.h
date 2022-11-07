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
	};

	using FrameGraphAssetInfoPtr = FrameGraphAssetInfo*;

	class SAILOR_API FrameGraphAssetInfoHandler final : public TSubmodule<FrameGraphAssetInfoHandler>, public IAssetInfoHandler
	{
	public:

		FrameGraphAssetInfoHandler(AssetRegistry* assetRegistry);

		virtual void GetDefaultMeta(YAML::Node& outDefaultYaml) const;
		virtual AssetInfoPtr CreateAssetInfo() const;

		virtual ~FrameGraphAssetInfoHandler() = default;
	};
}
