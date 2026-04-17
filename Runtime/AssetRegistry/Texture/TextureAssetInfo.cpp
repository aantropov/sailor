#include "TextureAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include "AssetRegistry/AssetRegistry.h"
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include <iostream>
#include "TextureImporter.h"
#include "Core/Reflection.h"

using namespace Sailor;

YAML::Node TextureAssetInfo::Serialize() const
{
	return SerializeReflectedAssetInfo(*this);
}

void TextureAssetInfo::Deserialize(const YAML::Node& outData)
{
	DeserializeReflectedAssetInfo(*this, outData);
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
