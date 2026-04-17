#include "WorldPrefabAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include "AssetRegistry/AssetRegistry.h"
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include "WorldPrefabImporter.h"
#include "Core/Reflection.h"
#include <iostream>

using namespace Sailor;

YAML::Node WorldPrefabAssetInfo::Serialize() const
{
	return SerializeReflectedAssetInfo(*this);
}

void WorldPrefabAssetInfo::Deserialize(const YAML::Node& outData)
{
	DeserializeReflectedAssetInfo(*this, outData);
}

WorldPrefabAssetInfoHandler::WorldPrefabAssetInfoHandler(AssetRegistry* assetRegistry)
{
	m_supportedExtensions.Emplace("world");
	assetRegistry->RegisterAssetInfoHandler(m_supportedExtensions, this);
}

void WorldPrefabAssetInfoHandler::GetDefaultMeta(YAML::Node& outDefaultYaml) const
{
	WorldPrefabAssetInfo defaultObject;
	outDefaultYaml = defaultObject.Serialize();
}

AssetInfoPtr WorldPrefabAssetInfoHandler::CreateAssetInfo() const
{
	return new WorldPrefabAssetInfo();
}

IAssetFactory* WorldPrefabAssetInfoHandler::GetFactory()
{
	return App::GetSubmodule<WorldPrefabImporter>();
}

IAssetInfoHandler* WorldPrefabAssetInfo::GetHandler()
{
	return App::GetSubmodule<WorldPrefabAssetInfoHandler>();
}
