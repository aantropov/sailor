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
	Sailor::SerializeArray(m_materials, outData["defaultMaterials"]);
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

	if (outData.contains("defaultMaterials"))
	{
		Sailor::DeserializeArray(m_materials, outData["defaultMaterials"]);
	}
}

ModelAssetInfoHandler::ModelAssetInfoHandler(AssetRegistry* assetRegistry)
{
	m_supportedExtensions.Emplace("obj");
	assetRegistry->RegisterAssetInfoHandler(m_supportedExtensions, this);
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