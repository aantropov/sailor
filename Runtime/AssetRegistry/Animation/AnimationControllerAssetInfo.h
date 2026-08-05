#pragma once

#include "AssetRegistry/AssetInfo.h"

namespace Sailor
{
	class AnimationControllerAssetInfo final : public AssetInfo
	{
		SAILOR_REFLECTABLE(AnimationControllerAssetInfo)

	public:
		SAILOR_API YAML::Node Serialize() const override;
		SAILOR_API void Deserialize(const YAML::Node& inData) override;
		SAILOR_API IAssetInfoHandler* GetHandler() override;
	};

	class AnimationSetAssetInfo final : public AssetInfo
	{
		SAILOR_REFLECTABLE(AnimationSetAssetInfo)

	public:
		SAILOR_API YAML::Node Serialize() const override;
		SAILOR_API void Deserialize(const YAML::Node& inData) override;
		SAILOR_API IAssetInfoHandler* GetHandler() override;
	};

	using AnimationControllerAssetInfoPtr = AnimationControllerAssetInfo*;
	using AnimationSetAssetInfoPtr = AnimationSetAssetInfo*;

	class AnimationControllerAssetInfoHandler final :
		public TSubmodule<AnimationControllerAssetInfoHandler>,
		public IAssetInfoHandler
	{
	public:
		SAILOR_API explicit AnimationControllerAssetInfoHandler(AssetRegistry* assetRegistry);
		SAILOR_API void GetDefaultMeta(YAML::Node& outDefaultYaml) const override;
		SAILOR_API AssetInfoPtr CreateAssetInfo() const override;
		SAILOR_API IAssetFactory* GetFactory() override;
	};

	class AnimationSetAssetInfoHandler final :
		public TSubmodule<AnimationSetAssetInfoHandler>,
		public IAssetInfoHandler
	{
	public:
		SAILOR_API explicit AnimationSetAssetInfoHandler(AssetRegistry* assetRegistry);
		SAILOR_API void GetDefaultMeta(YAML::Node& outDefaultYaml) const override;
		SAILOR_API AssetInfoPtr CreateAssetInfo() const override;
		SAILOR_API IAssetFactory* GetFactory() override;
	};
}

REFL_AUTO(
	type(Sailor::AnimationControllerAssetInfo, bases<Sailor::AssetInfo>),
	field(m_fileId),
	field(m_assetFilename)
)

REFL_AUTO(
	type(Sailor::AnimationSetAssetInfo, bases<Sailor::AssetInfo>),
	field(m_fileId),
	field(m_assetFilename)
)
