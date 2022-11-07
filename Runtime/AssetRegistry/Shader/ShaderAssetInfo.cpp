#include "ShaderAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include <iostream>

using namespace Sailor;

ShaderAssetInfoHandler::ShaderAssetInfoHandler(AssetRegistry* assetRegistry)
{
	m_supportedExtensions.Emplace("shader");
	m_supportedExtensions.Emplace("glsl");
	assetRegistry->RegisterAssetInfoHandler(m_supportedExtensions, this);
}

void ShaderAssetInfoHandler::GetDefaultMeta(YAML::Node& outDefaultYaml) const
{
	ShaderAssetInfo defaultObject;
	outDefaultYaml = defaultObject.Serialize();
}

AssetInfoPtr ShaderAssetInfoHandler::CreateAssetInfo() const
{
	return new ShaderAssetInfo();
}