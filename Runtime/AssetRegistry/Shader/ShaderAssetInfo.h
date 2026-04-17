#pragma once
#include "AssetRegistry/AssetInfo.h"
#include "Core/Singleton.hpp"

using namespace std;

namespace Sailor
{
	class ShaderAssetInfo final : public AssetInfo
	{
		SAILOR_REFLECTABLE(ShaderAssetInfo)

	public:
		SAILOR_API virtual ~ShaderAssetInfo() = default;

		SAILOR_API virtual IAssetInfoHandler* GetHandler() override;

	private:
	};

	using ShaderAssetInfoPtr = ShaderAssetInfo*;

	class ShaderAssetInfoHandler final : public TSubmodule<ShaderAssetInfoHandler>, public IAssetInfoHandler
	{

	public:

		IAssetFactory* GetFactory() override;

		SAILOR_API ShaderAssetInfoHandler(AssetRegistry* assetRegistry);

		SAILOR_API void GetDefaultMeta(YAML::Node& outDefaultYaml) const override;
		SAILOR_API AssetInfoPtr CreateAssetInfo() const override;

		SAILOR_API ~ShaderAssetInfoHandler() override = default;
	};
}

REFL_AUTO(
	type(Sailor::ShaderAssetInfo, bases<Sailor::AssetInfo>),
	field(m_fileId),
	field(m_assetFilename)
)
