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
	assetRegistry->RegisterAssetInfoHandler(m_supportedExtensions, this);
}

void ShaderAssetInfoHandler::GetDefaultMetaJson(nlohmann::json& outDefaultJson) const
{
	ShaderAssetInfo defaultObject;
	defaultObject.Serialize(outDefaultJson);
}

AssetInfoPtr ShaderAssetInfoHandler::CreateAssetInfo() const
{
	return new ShaderAssetInfo();
}