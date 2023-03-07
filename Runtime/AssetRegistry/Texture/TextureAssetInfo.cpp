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
	outData["format"] = SerializeEnum<RHI::ETextureFormat>(m_format);

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

	if (outData["format"])
	{
		DeserializeEnum<RHI::ETextureFormat>(outData["format"], m_format);
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
	m_supportedExtensions.Emplace("hdr");

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