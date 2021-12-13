#pragma once
#include "Core/Defines.h"
#include <string>
#include "Core/Vector.h"
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Core/Submodule.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "AssetRegistry/RenderPipeline/RenderPipelineAssetInfo.h"
#include "RHI/Renderer.h"

namespace Sailor
{
	class RenderPipelineImporter final : public TSubmodule<RenderPipelineImporter>, public IAssetInfoHandlerListener
	{
	public:
		using ByteCode = TVector<uint8_t>;

		SAILOR_API RenderPipelineImporter(RenderPipelineAssetInfoHandler* infoHandler);
		virtual SAILOR_API ~RenderPipelineImporter() override;

		virtual SAILOR_API void OnImportAsset(AssetInfoPtr assetInfo) override; 
		virtual SAILOR_API void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;
	};
}
