#include "AssetRegistry/Audio/AudioAssetInfo.h"
#include "AssetRegistry/Audio/AudioImporter.h"
#include "AssetRegistry/AssetRegistry.h"

using namespace Sailor;

YAML::Node AudioAssetInfo::Serialize() const
{
	return SerializeReflectedAssetInfo(*this);
}

void AudioAssetInfo::Deserialize(const YAML::Node& inData)
{
	DeserializeReflectedAssetInfo(*this, inData);
}

void AudioAssetInfo::CopyMetadata(const AssetInfo& source)
{
	CopyReflectedAssetInfo(*this, static_cast<const AudioAssetInfo&>(source));
}

IAssetInfoHandler* AudioAssetInfo::GetHandler()
{
	return App::GetSubmodule<AudioAssetInfoHandler>();
}

AudioAssetInfoHandler::AudioAssetInfoHandler(AssetRegistry* assetRegistry)
{
	m_supportedExtensions.Emplace("wav");
	m_supportedExtensions.Emplace("flac");
	m_supportedExtensions.Emplace("mp3");
	assetRegistry->RegisterAssetInfoHandler(m_supportedExtensions, this);
}

void AudioAssetInfoHandler::GetDefaultMeta(YAML::Node& outDefaultYaml) const
{
	AudioAssetInfo defaultObject;
	outDefaultYaml = defaultObject.Serialize();
}

AssetInfoPtr AudioAssetInfoHandler::CreateAssetInfo() const
{
	return new AudioAssetInfo();
}

IAssetFactory* AudioAssetInfoHandler::GetFactory()
{
	return App::GetSubmodule<AudioImporter>();
}
