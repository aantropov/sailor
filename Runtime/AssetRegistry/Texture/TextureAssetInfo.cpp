#include "TextureAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include <iostream>

using namespace Sailor;

YAML::Node TextureAssetInfo::Serialize() const
{
	YAML::Node outData;

	outData = AssetInfo::Serialize();
	outData["bShouldGenerateMips"] = m_bShouldGenerateMips;

	outData["clamping"] = SerializeEnum<RHI::ETextureClamping>(m_clamping);
	outData["filtration"] = SerializeEnum<RHI::ETextureFiltration>(m_filtration);

	return outData;
}

void TextureAssetInfo::Deserialize(const YAML::Node& outData)
{
	AssetInfo::Deserialize(outData);
	if (outData["clamping"])
	{
		DeserializeEnum<RHI::ETextureClamping>(outData["clamping"], m_clamping);
	}

	if (outData["filtration"])
	{
		DeserializeEnum<RHI::ETextureFiltration>(outData["filtration"], m_filtration);
	}

	if (outData["bShouldGenerateMips"])
	{
		m_bShouldGenerateMips = outData["bShouldGenerateMips"].as<bool>();
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
	m_supportedExtensions.Emplace("dds");

	assetRegistry->RegisterAssetInfoHandler(m_supportedExtensions, this);
}

void TextureAssetInfoHandler::GetDefaultMeta(YAML::Node& outDefaultYaml) const
{
	TextureAssetInfo defaultObject;
	outDefaultYaml = defaultObject.Serialize();
}

AssetInfoPtr TextureAssetInfoHandler::CreateAssetInfo() const
{
	return new TextureAssetInfo();
}