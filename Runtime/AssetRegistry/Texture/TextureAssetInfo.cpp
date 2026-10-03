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

void TextureAssetInfo::CopyMetadata(const AssetInfo& source)
{
	CopyReflectedAssetInfo(*this, static_cast<const TextureAssetInfo&>(source));
}

TextureAssetInfoHandler::TextureAssetInfoHandler(AssetRegistry* assetRegistry)
{
	assetRegistry->RegisterAssetInfoHandler(GetAssetInfoExtensions<TextureAssetInfo>(), this);
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
