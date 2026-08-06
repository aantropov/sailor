#pragma once

#include "AssetRegistry/AssetInfo.h"
#include "Core/Singleton.hpp"

namespace Sailor
{
	class AudioAssetInfo final : public AssetInfo
	{
		SAILOR_REFLECTABLE(AudioAssetInfo)

	public:
		SAILOR_API ~AudioAssetInfo() override = default;

		SAILOR_API YAML::Node Serialize() const override;
		SAILOR_API void Deserialize(const YAML::Node& inData) override;
		SAILOR_API IAssetInfoHandler* GetHandler() override;

		SAILOR_API bool ShouldStream() const { return m_stream; }

	private:
		bool m_stream = false;
	};

	using AudioAssetInfoPtr = AudioAssetInfo*;

	class AudioAssetInfoHandler final :
		public TSubmodule<AudioAssetInfoHandler>,
		public IAssetInfoHandler
	{
	public:
		SAILOR_API explicit AudioAssetInfoHandler(AssetRegistry* assetRegistry);
		SAILOR_API void GetDefaultMeta(YAML::Node& outDefaultYaml) const override;
		SAILOR_API AssetInfoPtr CreateAssetInfo() const override;
		SAILOR_API IAssetFactory* GetFactory() override;
	};
}

REFL_AUTO(
	type(Sailor::AudioAssetInfo, bases<Sailor::AssetInfo>),
	field(m_fileId),
	field(m_assetFilename),
	field(m_stream)
)
