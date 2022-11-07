#include "ModelAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include <iostream>

using namespace Sailor;

YAML::Node ModelAssetInfo::Serialize() const
{
	YAML::Node outData;
	outData = AssetInfo::Serialize();
	outData["bShouldGenerateMaterials"] = m_bShouldGenerateMaterials;
	outData["bShouldBatchByMaterial"] = m_bShouldBatchByMaterials;
	outData["defaultMaterials"] = m_materials;
	return outData;
}

void ModelAssetInfo::Deserialize(const YAML::Node& outData)
{
	AssetInfo::Deserialize(outData);

	if (outData["bShouldGenerateMaterials"])
	{
		m_bShouldGenerateMaterials = outData["bShouldGenerateMaterials"].as<bool>();;
	}

	if (outData["bShouldBatchByMaterial"])
	{
		m_bShouldBatchByMaterials = outData["bShouldBatchByMaterial"].as<bool>();;
	}

	if (outData["defaultMaterials"])
	{
		m_materials = outData["defaultMaterials"].as<TVector<UID>>();
	}
}

ModelAssetInfoHandler::ModelAssetInfoHandler(AssetRegistry* assetRegistry)
{
	m_supportedExtensions.Emplace("obj");
	assetRegistry->RegisterAssetInfoHandler(m_supportedExtensions, this);
}

void ModelAssetInfoHandler::GetDefaultMeta(YAML::Node& outDefaultYaml) const
{
	ModelAssetInfo defaultObject;
	outDefaultYaml = defaultObject.Serialize();
}

AssetInfoPtr ModelAssetInfoHandler::CreateAssetInfo() const
{
	return new ModelAssetInfo();
}