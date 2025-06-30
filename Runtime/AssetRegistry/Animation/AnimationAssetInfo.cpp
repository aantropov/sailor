#include "AnimationAssetInfo.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AnimationImporter.h"

using namespace Sailor;

YAML::Node AnimationAssetInfo::Serialize() const
{
	YAML::Node outData = AssetInfo::Serialize();
	SERIALIZE_PROPERTY(outData, m_animationIndex);
	SERIALIZE_PROPERTY(outData, m_skinIndex);
	return outData;
}

void AnimationAssetInfo::Deserialize(const YAML::Node& inData)
{
	AssetInfo::Deserialize(inData);
	DESERIALIZE_PROPERTY(inData, m_animationIndex);
	DESERIALIZE_PROPERTY(inData, m_skinIndex);
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
