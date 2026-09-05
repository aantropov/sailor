#include "AssetRegistry/GlobalIllumination/GIProbesAssetInfo.h"

#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/GlobalIllumination/GIProbesImporter.h"

using namespace Sailor;

YAML::Node GIProbesAssetInfo::Serialize() const
{
	return SerializeReflectedAssetInfo(*this);
}

void GIProbesAssetInfo::Deserialize(const YAML::Node& inData)
{
	DeserializeReflectedAssetInfo(*this, inData);
}

IAssetInfoHandler* GIProbesAssetInfo::GetHandler()
{
	return App::GetSubmodule<GIProbesAssetInfoHandler>();
}

GIProbesAssetInfoHandler::GIProbesAssetInfoHandler(
	AssetRegistry* assetRegistry)
{
	m_supportedExtensions.Emplace("probes");
	assetRegistry->RegisterAssetInfoHandler(m_supportedExtensions, this);
}

void GIProbesAssetInfoHandler::GetDefaultMeta(
	YAML::Node& outDefaultYaml) const
{
	GIProbesAssetInfo defaultObject;
	outDefaultYaml = defaultObject.Serialize();
}

AssetInfoPtr GIProbesAssetInfoHandler::CreateAssetInfo() const
{
	return new GIProbesAssetInfo();
}

IAssetFactory* GIProbesAssetInfoHandler::GetFactory()
{
	return App::GetSubmodule<GIProbesImporter>();
}
