#include "ShaderAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include "AssetRegistry/AssetRegistry.h"
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include <iostream>
#include "ShaderCompiler.h"

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

IAssetInfoHandler* ShaderAssetInfo::GetHandler()
{
	return App::GetSubmodule<ShaderAssetInfoHandler>();
}

AssetInfoPtr ShaderAssetInfoHandler::CreateAssetInfo() const
{
	return new ShaderAssetInfo();
}

IAssetFactory* ShaderAssetInfoHandler::GetFactory()
{
	return App::GetSubmodule<ShaderCompiler>();
}