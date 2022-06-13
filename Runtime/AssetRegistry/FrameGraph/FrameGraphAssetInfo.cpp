#include "FrameGraphAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include <iostream>

using namespace Sailor;

void FrameGraphAssetInfo::Serialize(nlohmann::json& outData) const
{
	AssetInfo::Serialize(outData);
}

void FrameGraphAssetInfo::Deserialize(const nlohmann::json& outData)
{
	AssetInfo::Deserialize(outData);
}

FrameGraphAssetInfoHandler::FrameGraphAssetInfoHandler(AssetRegistry* assetRegistry)
{
	m_supportedExtensions.Emplace("renderer");
	assetRegistry->RegisterAssetInfoHandler(m_supportedExtensions, this);
}

void FrameGraphAssetInfoHandler::GetDefaultMetaJson(nlohmann::json& outDefaultJson) const
{
	FrameGraphAssetInfo defaultObject;
	defaultObject.Serialize(outDefaultJson);
}

AssetInfoPtr FrameGraphAssetInfoHandler::CreateAssetInfo() const
{
	return new FrameGraphAssetInfo();
}