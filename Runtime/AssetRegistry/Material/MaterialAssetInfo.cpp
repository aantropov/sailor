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
	m_pInstance = new MaterialAssetInfoHandler();

	m_pInstance->m_supportedExtensions.emplace_back("mat");
	EngineInstance::GetSubmodule<AssetRegistry>()->RegisterAssetInfoHandler(m_pInstance->m_supportedExtensions, m_pInstance);
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