#pragma once
#include "AssetRegistry/AssetInfo.h"
#include "RHI/Types.h"
#include "Core/Singleton.hpp"

using namespace std;

namespace Sailor
{
	class FrameGraphAssetInfo final : public AssetInfo
	{
		SAILOR_REFLECTABLE(FrameGraphAssetInfo)

	public:
		SAILOR_API virtual ~FrameGraphAssetInfo() = default;
		IAssetInfoHandler* GetHandler() override;

	private:
	};

	using FrameGraphAssetInfoPtr = FrameGraphAssetInfo*;

	class SAILOR_API FrameGraphAssetInfoHandler final : public TSubmodule<FrameGraphAssetInfoHandler>, public IAssetInfoHandler
	{
	public:

		FrameGraphAssetInfoHandler(AssetRegistry* assetRegistry);

		virtual void GetDefaultMeta(YAML::Node& outDefaultYaml) const override;
		AssetInfoPtr CreateAssetInfo() const override;

		virtual ~FrameGraphAssetInfoHandler() = default;
	};
}

REFL_AUTO(
	type(Sailor::FrameGraphAssetInfo, bases<Sailor::AssetInfo>),
	field(m_fileId),
	field(m_assetFilename)
)
