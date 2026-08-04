#include "AnimationControllerAssetInfo.h"
#include "AnimationControllerImporter.h"
#include "AssetRegistry/AssetRegistry.h"

using namespace Sailor;

YAML::Node AnimationControllerAssetInfo::Serialize() const
{
	return SerializeReflectedAssetInfo(*this);
}

void AnimationControllerAssetInfo::Deserialize(const YAML::Node& inData)
{
	DeserializeReflectedAssetInfo(*this, inData);
}

YAML::Node AnimationSetAssetInfo::Serialize() const
{
	return SerializeReflectedAssetInfo(*this);
}

void AnimationSetAssetInfo::Deserialize(const YAML::Node& inData)
{
	DeserializeReflectedAssetInfo(*this, inData);
}

AnimationControllerAssetInfoHandler::AnimationControllerAssetInfoHandler(
	AssetRegistry* assetRegistry)
{
	m_supportedExtensions.Emplace("animcontroller");
	assetRegistry->RegisterAssetInfoHandler(m_supportedExtensions, this);
}

void AnimationControllerAssetInfoHandler::GetDefaultMeta(
	YAML::Node& outDefaultYaml) const
{
	AnimationControllerAssetInfo defaultObject;
	outDefaultYaml = defaultObject.Serialize();
}

AssetInfoPtr AnimationControllerAssetInfoHandler::CreateAssetInfo() const
{
	return new AnimationControllerAssetInfo();
}

IAssetFactory* AnimationControllerAssetInfoHandler::GetFactory()
{
	return App::GetSubmodule<AnimationControllerImporter>();
}

IAssetInfoHandler* AnimationControllerAssetInfo::GetHandler()
{
	return App::GetSubmodule<AnimationControllerAssetInfoHandler>();
}

AnimationSetAssetInfoHandler::AnimationSetAssetInfoHandler(
	AssetRegistry* assetRegistry)
{
	m_supportedExtensions.Emplace("animset");
	assetRegistry->RegisterAssetInfoHandler(m_supportedExtensions, this);
}

void AnimationSetAssetInfoHandler::GetDefaultMeta(YAML::Node& outDefaultYaml) const
{
	AnimationSetAssetInfo defaultObject;
	outDefaultYaml = defaultObject.Serialize();
}

AssetInfoPtr AnimationSetAssetInfoHandler::CreateAssetInfo() const
{
	return new AnimationSetAssetInfo();
}

IAssetFactory* AnimationSetAssetInfoHandler::GetFactory()
{
	return App::GetSubmodule<AnimationControllerImporter>();
}

IAssetInfoHandler* AnimationSetAssetInfo::GetHandler()
{
	return App::GetSubmodule<AnimationSetAssetInfoHandler>();
}
