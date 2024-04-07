#include "PrefabAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include <iostream>

using namespace Sailor;

YAML::Node PrefabAssetInfo::Serialize() const
{
	YAML::Node outData;
	outData = AssetInfo::Serialize();
	return outData;
}

void PrefabAssetInfo::Deserialize(const YAML::Node& outData)
{
	AssetInfo::Deserialize(outData);
}

PrefabAssetInfoHandler::PrefabAssetInfoHandler(AssetRegistry* assetRegistry)
{
	m_supportedExtensions.Emplace("prefab");
	assetRegistry->RegisterAssetInfoHandler(m_supportedExtensions, this);
}

void PrefabAssetInfoHandler::GetDefaultMeta(YAML::Node& outDefaultYaml) const
{
	PrefabAssetInfo defaultObject;
	outDefaultYaml = defaultObject.Serialize();
}

AssetInfoPtr PrefabAssetInfoHandler::CreateAssetInfo() const
{
	return new PrefabAssetInfo();
}