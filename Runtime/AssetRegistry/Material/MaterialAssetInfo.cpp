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

void MaterialAssetInfoHandler::Initialize()
{
	s_pInstance = new MaterialAssetInfoHandler();

	s_pInstance->m_supportedExtensions.emplace_back("mat");
	App::GetSubmodule<AssetRegistry>()->RegisterAssetInfoHandler(s_pInstance->m_supportedExtensions, s_pInstance);
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