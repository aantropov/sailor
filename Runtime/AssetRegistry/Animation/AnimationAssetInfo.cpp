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

IAssetInfoHandler* AnimationAssetInfo::GetHandler()
{
	return App::GetSubmodule<AnimationAssetInfoHandler>();
}

AnimationAssetInfoHandler::AnimationAssetInfoHandler(AssetRegistry* assetRegistry)
{
	m_supportedExtensions.Emplace("gltf");
	m_supportedExtensions.Emplace("glb");
	m_supportedExtensions.Emplace("anim");
	assetRegistry->RegisterAssetInfoHandler(m_supportedExtensions, this);
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
