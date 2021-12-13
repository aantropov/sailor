#include "MaterialAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include <iostream>

using namespace Sailor;

void MaterialAssetInfo::Serialize(nlohmann::json& outData) const
{
	AssetInfo::Serialize(outData);
}

void MaterialAssetInfo::Deserialize(const nlohmann::json& outData)
{
	AssetInfo::Deserialize(outData);
}

MaterialAssetInfoHandler::MaterialAssetInfoHandler(AssetRegistry* assetRegistry)
{
	m_supportedExtensions.Emplace("mat");
	assetRegistry->RegisterAssetInfoHandler(m_supportedExtensions, this);
}

void MaterialAssetInfoHandler::GetDefaultMetaJson(nlohmann::json& outDefaultJson) const
{
	MaterialAssetInfo defaultObject;
	defaultObject.Serialize(outDefaultJson);
}

AssetInfoPtr MaterialAssetInfoHandler::CreateAssetInfo() const
{
	return new MaterialAssetInfo();
}