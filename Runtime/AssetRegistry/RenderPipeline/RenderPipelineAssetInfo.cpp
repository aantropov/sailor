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

RenderPipelineAssetInfoHandler::RenderPipelineAssetInfoHandler(AssetRegistry* assetRegistry)
{
	m_supportedExtensions.Emplace("render_pipeline");
	assetRegistry->RegisterAssetInfoHandler(m_supportedExtensions, this);
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