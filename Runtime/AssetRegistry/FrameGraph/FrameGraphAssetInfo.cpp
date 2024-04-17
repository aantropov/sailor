#include "FrameGraphAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include "AssetRegistry/AssetRegistry.h"
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include <iostream>

using namespace Sailor;

FrameGraphAssetInfoHandler::FrameGraphAssetInfoHandler(AssetRegistry* assetRegistry)
{
	m_supportedExtensions.Emplace("renderer");
	assetRegistry->RegisterAssetInfoHandler(m_supportedExtensions, this);
}

void FrameGraphAssetInfoHandler::GetDefaultMeta(YAML::Node& outDefaultYaml) const
{
	FrameGraphAssetInfo defaultObject;
	outDefaultYaml = defaultObject.Serialize();
}

AssetInfoPtr FrameGraphAssetInfoHandler::CreateAssetInfo() const
{
	return new FrameGraphAssetInfo();
}