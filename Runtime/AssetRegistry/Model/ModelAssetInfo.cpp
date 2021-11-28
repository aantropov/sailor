#include "ModelAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include <iostream>

using namespace Sailor;

void ModelAssetInfo::Serialize(nlohmann::json& outData) const
{
	AssetInfo::Serialize(outData);
	outData["generate_materials"] = m_bShouldGenerateMaterials;
	outData["batch_by_material"] = m_bShouldBatchByMaterials;
}

void ModelAssetInfo::Deserialize(const nlohmann::json& outData)
{
	AssetInfo::Deserialize(outData);
	
	if (outData.contains("generate_materials"))
	{
		m_bShouldGenerateMaterials = outData["generate_materials"].get<bool>();;
	}
	
	if (outData.contains("batch_by_material"))
	{
		m_bShouldBatchByMaterials = outData["batch_by_material"].get<bool>();;
	}
}

void ModelAssetInfoHandler::Initialize()
{
	s_pInstance = new ModelAssetInfoHandler();

	s_pInstance->m_supportedExtensions.emplace_back("obj");
	App::GetSubmodule<AssetRegistry>()->RegisterAssetInfoHandler(s_pInstance->m_supportedExtensions, s_pInstance);
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