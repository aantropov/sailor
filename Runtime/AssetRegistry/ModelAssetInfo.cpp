#include "ModelAssetInfo.h"
#include "AssetInfo.h"
#include <filesystem>
#include <fstream>
#include "Utils.h"
#include <iostream>

using namespace Sailor;

void ModelAssetInfo::Serialize(nlohmann::json& outData) const
{
	AssetInfo::Serialize(outData);
	outData["generate_materials"] = m_bShouldGenerateMaterials;
}

void ModelAssetInfo::Deserialize(const nlohmann::json& outData)
{
	AssetInfo::Deserialize(outData);
	
	if (outData.contains("generate_materials"))
	{
		m_bShouldGenerateMaterials = outData["generate_materials"].get<bool>();;
	}
}

void ModelAssetInfoHandler::Initialize()
{
	m_pInstance = new ModelAssetInfoHandler();

	m_pInstance->m_supportedExtensions.emplace_back("obj");
	AssetRegistry::GetInstance()->RegisterAssetInfoHandler(m_pInstance->m_supportedExtensions, m_pInstance);
}

void ModelAssetInfoHandler::GetDefaultMetaJson(nlohmann::json& outDefaultJson) const
{
	ModelAssetInfo defaultObject;
	defaultObject.Serialize(outDefaultJson);
}

AssetInfoPtr ModelAssetInfoHandler::CreateAssetInfo() const
{
	return new ModelAssetInfo();
}