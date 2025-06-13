#include "ModelAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/AssetRegistry.h"
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include <iostream>

using namespace Sailor;

YAML::Node ModelAssetInfo::Serialize() const
{
	YAML::Node outData;
	outData = AssetInfo::Serialize();

	SERIALIZE_PROPERTY(outData, m_bShouldGenerateMaterials);
	SERIALIZE_PROPERTY(outData, m_bShouldBatchByMaterial);
	SERIALIZE_PROPERTY(outData, m_unitScale);
	SERIALIZE_PROPERTY(outData, m_materials);
	SERIALIZE_PROPERTY(outData, m_animations);

	return outData;
}

void ModelAssetInfo::Deserialize(const YAML::Node& outData)
{
	AssetInfo::Deserialize(outData);

	DESERIALIZE_PROPERTY(outData, m_bShouldGenerateMaterials);
	DESERIALIZE_PROPERTY(outData, m_bShouldBatchByMaterial);
	DESERIALIZE_PROPERTY(outData, m_unitScale);
	DESERIALIZE_PROPERTY(outData, m_materials);
	DESERIALIZE_PROPERTY(outData, m_animations);
}

ModelAssetInfoHandler::ModelAssetInfoHandler(AssetRegistry* assetRegistry)
{
	// TODO: Add more formats
	m_supportedExtensions.Emplace("glb");
	m_supportedExtensions.Emplace("gltf");
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

IAssetFactory* ModelAssetInfoHandler::GetFactory()
{
	return App::GetSubmodule<ModelImporter>();
}