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
	outData["generate_mips"] = m_bShouldGenerateMips;
	outData["clamping"] = (uint8_t)m_clamping;
	outData["filtration"] = (uint8_t)m_filtration;
}

void TextureAssetInfo::Deserialize(const nlohmann::json& outData)
{
	AssetInfo::Deserialize(outData);
	if (outData.contains("clamping"))
	{
		m_clamping = (RHI::ETextureClamping)outData["clamping"].get<uint8_t>();
	}

	if (outData.contains("filtration"))
	{
		m_filtration = (RHI::ETextureFiltration)outData["filtration"].get<uint8_t>();
	}

	if (outData.contains("generate_mips"))
	{
		m_bShouldGenerateMips = outData["generate_mips"].get<bool>();
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

AssetInfoPtr TextureAssetInfoHandler::CreateAssetInfo() const
{
	return new TextureAssetInfo();
}