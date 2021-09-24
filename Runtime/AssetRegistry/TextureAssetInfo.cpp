#include "TextureAssetInfo.h"
#include "AssetInfo.h"
#include <filesystem>
#include <fstream>
#include "Utils.h"
#include <iostream>

using namespace Sailor;

void TextureAssetInfo::Serialize(nlohmann::json& outData) const
{
	AssetInfo::Serialize(outData);
	outData["generate_mips"] = bGenerateMips;
}

void TextureAssetInfo::Deserialize(const nlohmann::json& outData)
{
	AssetInfo::Deserialize(outData);
	if (outData.contains("generate_mips"))
	{
		bGenerateMips = outData["generate_mips"].get<bool>();
	}
}

void TextureAssetInfoHandler::Initialize()
{
	m_pInstance = new TextureAssetInfoHandler();

	m_pInstance->m_supportedExtensions.emplace_back("png");
	m_pInstance->m_supportedExtensions.emplace_back("bmp");
	m_pInstance->m_supportedExtensions.emplace_back("tga");
	m_pInstance->m_supportedExtensions.emplace_back("jpg");
	m_pInstance->m_supportedExtensions.emplace_back("gif");
	m_pInstance->m_supportedExtensions.emplace_back("psd");
	AssetRegistry::GetInstance()->RegisterAssetInfoHandler(m_pInstance->m_supportedExtensions, m_pInstance);
}

void TextureAssetInfoHandler::GetDefaultMetaJson(nlohmann::json& outDefaultJson) const
{
	TextureAssetInfo defaultObject;
	defaultObject.Serialize(outDefaultJson);
}

AssetInfo* TextureAssetInfoHandler::CreateAssetInfo() const
{
	return new TextureAssetInfo();
}