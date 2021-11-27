#include "RenderPipelineAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include <iostream>

using namespace Sailor;

void RenderPipelineAssetInfo::Serialize(nlohmann::json& outData) const
{
	AssetInfo::Serialize(outData);
}

void RenderPipelineAssetInfo::Deserialize(const nlohmann::json& outData)
{
	AssetInfo::Deserialize(outData);
}

void RenderPipelineAssetInfoHandler::Initialize()
{
	m_pInstance = new RenderPipelineAssetInfoHandler();

	m_pInstance->m_supportedExtensions.emplace_back("render_pipeline");
	AssetRegistry::GetInstance()->RegisterAssetInfoHandler(m_pInstance->m_supportedExtensions, m_pInstance);
}

void RenderPipelineAssetInfoHandler::GetDefaultMetaJson(nlohmann::json& outDefaultJson) const
{
	RenderPipelineAssetInfo defaultObject;
	defaultObject.Serialize(outDefaultJson);
}

AssetInfoPtr RenderPipelineAssetInfoHandler::CreateAssetInfo() const
{
	return new RenderPipelineAssetInfo();
}