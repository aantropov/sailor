#include "ShaderAssetInfo.h"
#include "AssetInfo.h"
#include <filesystem>
#include <fstream>
#include "Utils.h"
#include <iostream>

using namespace Sailor;

void ShaderAssetInfoHandler::Initialize()
{
	m_pInstance = new ShaderAssetInfoHandler();

	m_pInstance->m_supportedExtensions.emplace_back("shader");
	AssetRegistry::GetInstance()->RegisterAssetInfoHandler(m_pInstance->m_supportedExtensions, m_pInstance);
}

void ShaderAssetInfoHandler::GetDefaultMetaJson(nlohmann::json& outDefaultJson) const
{
	ShaderAssetInfo defaultObject;
	defaultObject.Serialize(outDefaultJson);
}

AssetInfo* ShaderAssetInfoHandler::CreateAssetInfo() const
{
	return new ShaderAssetInfo();
}