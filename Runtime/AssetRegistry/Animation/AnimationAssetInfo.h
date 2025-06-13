	#pragma once
	#include "AssetRegistry/AssetInfo.h"
	#include "Core/Singleton.hpp"
	
	namespace Sailor
	{
class AnimationAssetInfo final : public AssetInfo
{
public:
SAILOR_API virtual ~AnimationAssetInfo() = default;

SAILOR_API virtual YAML::Node Serialize() const override;
SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;

SAILOR_API int32_t GetAnimationIndex() const { return m_animationIndex; }
SAILOR_API int32_t GetSkinIndex() const { return m_skinIndex; }

private:
int32_t m_animationIndex = 0;
int32_t m_skinIndex = 0;
};
	
	using AnimationAssetInfoPtr = AnimationAssetInfo*;
	
	class AnimationAssetInfoHandler final : public TSubmodule<AnimationAssetInfoHandler>, public IAssetInfoHandler
	{
	public:
	SAILOR_API AnimationAssetInfoHandler(AssetRegistry* assetRegistry);
	SAILOR_API virtual void GetDefaultMeta(YAML::Node& outDefaultYaml) const override;
	SAILOR_API virtual AssetInfoPtr CreateAssetInfo() const override;
	IAssetFactory* GetFactory() override;
	};
	}
