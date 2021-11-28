#pragma once
#include "AssetRegistry/AssetInfo.h"
#include "RHI/Types.h"
#include "Core/Singleton.hpp"

using namespace std;

namespace Sailor
{
	class RenderPipelineAssetInfo final : public AssetInfo
	{
	public:
		virtual SAILOR_API ~RenderPipelineAssetInfo() = default;

		virtual SAILOR_API void Serialize(nlohmann::json& outData) const override;
		virtual SAILOR_API void Deserialize(const nlohmann::json& inData) override;

	};

	using RenderPipelineAssetInfoPtr = RenderPipelineAssetInfo*;

	class RenderPipelineAssetInfoHandler final : public TSubmodule<RenderPipelineAssetInfoHandler>, public IAssetInfoHandler
	{

	public:

		SAILOR_API RenderPipelineAssetInfoHandler(AssetRegistry* assetRegistry);

		virtual SAILOR_API void GetDefaultMetaJson(nlohmann::json& outDefaultJson) const;
		virtual SAILOR_API AssetInfoPtr CreateAssetInfo() const;

		virtual SAILOR_API ~RenderPipelineAssetInfoHandler() = default;
	};
}
