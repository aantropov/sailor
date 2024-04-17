#include "MaterialAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include <iostream>
#include "MaterialImporter.h"
#include "AssetRegistry/AssetRegistry.h"

using namespace Sailor;

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

AssetInfoPtr MaterialAssetInfoHandler::CreateAssetInfo() const
{
	return new MaterialAssetInfo();
}

IAssetFactory* MaterialAssetInfoHandler::GetFactory()
{
	return App::GetSubmodule<MaterialImporter>();
}