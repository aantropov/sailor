#include "ShaderAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include <iostream>

using namespace Sailor;

void ShaderAssetInfoHandler::Initialize()
{
	m_pInstance = new ShaderAssetInfoHandler();

	m_pInstance->m_supportedExtensions.emplace_back("shader");
	EngineInstance::GetSubmodule<AssetRegistry>()->RegisterAssetInfoHandler(m_pInstance->m_supportedExtensions, m_pInstance);
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