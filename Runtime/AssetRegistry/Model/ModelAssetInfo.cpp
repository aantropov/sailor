#include "ModelAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/AssetRegistry.h"
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include "Core/Reflection.h"
#include <iostream>

using namespace Sailor;

YAML::Node ModelAssetInfo::Serialize() const
{
	return SerializeReflectedAssetInfo(*this);
}

void ModelAssetInfo::Deserialize(const YAML::Node& outData)
{
	DeserializeReflectedAssetInfo(*this, outData);
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

IAssetInfoHandler* ModelAssetInfo::GetHandler()
{
	return App::GetSubmodule<ModelAssetInfoHandler>();
}

AssetInfoPtr ModelAssetInfoHandler::CreateAssetInfo() const
{
	return new ModelAssetInfo();
}

IAssetFactory* ModelAssetInfoHandler::GetFactory()
{
	return App::GetSubmodule<ModelImporter>();
}
