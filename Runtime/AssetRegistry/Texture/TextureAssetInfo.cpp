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
	outData["clamping"] = magic_enum::enum_name(m_clamping);
	outData["filtration"] = magic_enum::enum_name(m_filtration);
}

void TextureAssetInfo::Deserialize(const nlohmann::json& outData)
{
	AssetInfo::Deserialize(outData);
	if (outData.contains("clamping"))
	{
		m_clamping = magic_enum::enum_cast<RHI::ETextureClamping>(outData["clamping"].get<std::string>()).value();
	}

	if (outData.contains("filtration"))
	{
		m_filtration = magic_enum::enum_cast<RHI::ETextureFiltration>(outData["filtration"].get<std::string>()).value();
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