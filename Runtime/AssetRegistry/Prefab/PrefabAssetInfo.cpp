#include "PrefabAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include "AssetRegistry/AssetRegistry.h"
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include "PrefabImporter.h"
#include "Core/Reflection.h"
#include <iostream>

using namespace Sailor;

YAML::Node PrefabAssetInfo::Serialize() const
{
	return SerializeReflectedAssetInfo(*this);
}

void PrefabAssetInfo::Deserialize(const YAML::Node& outData)
{
	DeserializeReflectedAssetInfo(*this, outData);
}

IAssetInfoHandler* PrefabAssetInfo::GetHandler()
{
	return App::GetSubmodule<PrefabAssetInfoHandler>();
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

IAssetFactory* PrefabAssetInfoHandler::GetFactory()
{
	return App::GetSubmodule<PrefabImporter>();
}
