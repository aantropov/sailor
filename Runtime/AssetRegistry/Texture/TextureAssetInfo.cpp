#include "TextureAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include <iostream>

using namespace Sailor;

void TextureAssetInfo::Serialize(nlohmann::json& outData) const
{
	AssetInfo::Serialize(outData);
	outData["bShouldGenerateMips"] = m_bShouldGenerateMips;

	SerializeEnum<RHI::ETextureClamping>(outData["clamping"], m_clamping);
	SerializeEnum<RHI::ETextureFiltration>(outData["filtration"], m_filtration);
}

void TextureAssetInfo::Deserialize(const nlohmann::json& outData)
{
	AssetInfo::Deserialize(outData);
	if (outData.contains("clamping"))
	{
		DeserializeEnum<RHI::ETextureClamping>(outData["clamping"], m_clamping);
	}

	if (outData.contains("filtration"))
	{
		DeserializeEnum<RHI::ETextureFiltration>(outData["filtration"], m_filtration);
	}

	if (outData.contains("bShouldGenerateMips"))
	{
		m_bShouldGenerateMips = outData["bShouldGenerateMips"].get<bool>();
	}
}

TextureAssetInfoHandler::TextureAssetInfoHandler(AssetRegistry* assetRegistry)
{
	m_supportedExtensions.Emplace("png");
	m_supportedExtensions.Emplace("bmp");
	m_supportedExtensions.Emplace("tga");
	m_supportedExtensions.Emplace("jpg");
	m_supportedExtensions.Emplace("gif");
	m_supportedExtensions.Emplace("psd");

	assetRegistry->RegisterAssetInfoHandler(m_supportedExtensions, this);
}

void TextureAssetInfoHandler::GetDefaultMetaJson(nlohmann::json& outDefaultJson) const
{
	TextureAssetInfo defaultObject;
	defaultObject.Serialize(outDefaultJson);
}

AssetInfoPtr TextureAssetInfoHandler::CreateAssetInfo() const
{
	return new TextureAssetInfo();
}