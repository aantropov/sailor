#include "AnimationAssetInfo.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AnimationImporter.h"
#include "Core/Reflection.h"

using namespace Sailor;

YAML::Node AnimationAssetInfo::Serialize() const
{
	return SerializeReflectedAssetInfo(*this);
}

void AnimationAssetInfo::Deserialize(const YAML::Node& inData)
{
	DeserializeReflectedAssetInfo(*this, inData);
}

void AnimationAssetInfo::CopyMetadata(const AssetInfo& source)
{
	CopyReflectedAssetInfo(*this, static_cast<const AnimationAssetInfo&>(source));
}

IAssetInfoHandler* AnimationAssetInfo::GetHandler()
{
	return App::GetSubmodule<AnimationAssetInfoHandler>();
}

AnimationAssetInfoHandler::AnimationAssetInfoHandler(AssetRegistry* assetRegistry)
{
	assetRegistry->RegisterAssetInfoHandler(GetAssetInfoExtensions<AnimationAssetInfo>(), this);
}

void AnimationAssetInfoHandler::GetDefaultMeta(YAML::Node& outDefaultYaml) const
{
	AnimationAssetInfo defaultObject;
	outDefaultYaml = defaultObject.Serialize();
}

AssetInfoPtr AnimationAssetInfoHandler::CreateAssetInfo() const
{
	return new AnimationAssetInfo();
}

IAssetFactory* AnimationAssetInfoHandler::GetFactory()
{
	return App::GetSubmodule<AnimationImporter>();
}
