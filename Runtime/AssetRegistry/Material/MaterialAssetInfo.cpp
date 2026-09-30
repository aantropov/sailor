#include "MaterialAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include <iostream>
#include "MaterialImporter.h"
#include "AssetRegistry/AssetRegistry.h"
#include "Core/Reflection.h"

using namespace Sailor;

YAML::Node MaterialAssetInfo::Serialize() const
{
	return SerializeReflectedAssetInfo(*this);
}

void MaterialAssetInfo::Deserialize(const YAML::Node& inData)
{
	DeserializeReflectedAssetInfo(*this, inData);
}

void MaterialAssetInfo::CopyMetadata(const AssetInfo& source)
{
	CopyReflectedAssetInfo(*this, static_cast<const MaterialAssetInfo&>(source));
}

MaterialAssetInfoHandler::MaterialAssetInfoHandler(AssetRegistry* assetRegistry)
{
	m_supportedExtensions.Emplace("mat");
	assetRegistry->RegisterAssetInfoHandler(m_supportedExtensions, this);
}

void MaterialAssetInfoHandler::GetDefaultMeta(YAML::Node& outDefaultYaml) const
{
	MaterialAssetInfo defaultObject;
	outDefaultYaml = defaultObject.Serialize();
}

IAssetInfoHandler* MaterialAssetInfo::GetHandler()
{
	return App::GetSubmodule<MaterialAssetInfoHandler>();
}

AssetInfoPtr MaterialAssetInfoHandler::CreateAssetInfo() const
{
	return new MaterialAssetInfo();
}

IAssetFactory* MaterialAssetInfoHandler::GetFactory()
{
	return App::GetSubmodule<MaterialImporter>();
}
