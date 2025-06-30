#include "TextureAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include "AssetRegistry/AssetRegistry.h"
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include <iostream>
#include "TextureImporter.h"

using namespace Sailor;

YAML::Node TextureAssetInfo::Serialize() const
{
	YAML::Node outData;

	outData = AssetInfo::Serialize();

	SERIALIZE_PROPERTY(outData, m_clamping);
	SERIALIZE_PROPERTY(outData, m_reduction);
	SERIALIZE_PROPERTY(outData, m_filtration);
	SERIALIZE_PROPERTY(outData, m_bShouldGenerateMips);
	SERIALIZE_PROPERTY(outData, m_bShouldSupportStorageBinding);
	SERIALIZE_PROPERTY(outData, m_format);
	SERIALIZE_PROPERTY(outData, m_glbTextureIndex);

	return outData;
}

void TextureAssetInfo::Deserialize(const YAML::Node& outData)
{
	AssetInfo::Deserialize(outData);

	DESERIALIZE_PROPERTY(outData, m_clamping);
	DESERIALIZE_PROPERTY(outData, m_reduction);
	DESERIALIZE_PROPERTY(outData, m_filtration);
	DESERIALIZE_PROPERTY(outData, m_bShouldGenerateMips);
	DESERIALIZE_PROPERTY(outData, m_bShouldSupportStorageBinding);
	DESERIALIZE_PROPERTY(outData, m_format);
	DESERIALIZE_PROPERTY(outData, m_glbTextureIndex);
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

IAssetInfoHandler* TextureAssetInfo::GetHandler()
{
	return App::GetSubmodule<TextureAssetInfoHandler>();
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

IAssetFactory* TextureAssetInfoHandler::GetFactory()
{
	return App::GetSubmodule<TextureImporter>();
}