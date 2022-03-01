#pragma once
#include "Core/Defines.h"
#include <string>
#include "Containers/Vector.h"
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Core/Submodule.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "AssetRegistry/RenderPipeline/RenderPipelineAssetInfo.h"
#include "RHI/Renderer.h"
#include "RenderPipeline/RenderPipeline.h"

namespace Sailor
{
	class SAILOR_API RenderPipelineImporter final : public TSubmodule<RenderPipelineImporter>, public IAssetInfoHandlerListener
	{
	public:
		using ByteCode = TVector<uint8_t>;

		RenderPipelineImporter(RenderPipelineAssetInfoHandler* infoHandler);
		virtual ~RenderPipelineImporter() override;

		virtual void OnImportAsset(AssetInfoPtr assetInfo) override;
		virtual void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;
	};
}
