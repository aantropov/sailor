#include "AssetRegistry/GlobalIllumination/ProbeVolumeAssetInfo.h"

#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/GlobalIllumination/ProbeVolumeImporter.h"

using namespace Sailor;

YAML::Node ProbeVolumeAssetInfo::Serialize() const
{
	return SerializeReflectedAssetInfo(*this);
}

void ProbeVolumeAssetInfo::Deserialize(const YAML::Node& inData)
{
	DeserializeReflectedAssetInfo(*this, inData);
}

IAssetInfoHandler* ProbeVolumeAssetInfo::GetHandler()
{
	return App::GetSubmodule<ProbeVolumeAssetInfoHandler>();
}

ProbeVolumeAssetInfoHandler::ProbeVolumeAssetInfoHandler(
	AssetRegistry* assetRegistry)
{
	m_supportedExtensions.Emplace("probes");
	assetRegistry->RegisterAssetInfoHandler(m_supportedExtensions, this);
}

void ProbeVolumeAssetInfoHandler::GetDefaultMeta(
	YAML::Node& outDefaultYaml) const
{
	ProbeVolumeAssetInfo defaultObject;
	outDefaultYaml = defaultObject.Serialize();
}

AssetInfoPtr ProbeVolumeAssetInfoHandler::CreateAssetInfo() const
{
	return new ProbeVolumeAssetInfo();
}

IAssetFactory* ProbeVolumeAssetInfoHandler::GetFactory()
{
	return App::GetSubmodule<ProbeVolumeImporter>();
}
