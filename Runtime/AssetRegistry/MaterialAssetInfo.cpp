#include "MaterialAssetInfo.h"
#include "AssetInfo.h"
#include <filesystem>
#include <fstream>
#include "Utils.h"
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
	m_pInstance = new MaterialAssetInfoHandler();

	m_pInstance->m_supportedExtensions.emplace_back("mat");
	AssetRegistry::GetInstance()->RegisterAssetInfoHandler(m_pInstance->m_supportedExtensions, m_pInstance);
}

void MaterialAssetInfoHandler::GetDefaultMetaJson(nlohmann::json& outDefaultJson) const
{
	MaterialAssetInfo defaultObject;
	defaultObject.Serialize(outDefaultJson);
}

AssetInfo* MaterialAssetInfoHandler::CreateAssetInfo() const
{
	return new MaterialAssetInfo();
}