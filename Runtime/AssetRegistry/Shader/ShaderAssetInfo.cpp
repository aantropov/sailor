#include "ShaderAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include <iostream>

using namespace Sailor;

void ShaderAssetInfoHandler::Initialize()
{
	s_pInstance = new ShaderAssetInfoHandler();

	s_pInstance->m_supportedExtensions.emplace_back("shader");
	App::GetSubmodule<AssetRegistry>()->RegisterAssetInfoHandler(s_pInstance->m_supportedExtensions, s_pInstance);
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