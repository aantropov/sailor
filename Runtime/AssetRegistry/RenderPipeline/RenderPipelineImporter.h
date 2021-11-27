#pragma once
#include "Core/Defines.h"
#include <string>
#include <vector>
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Core/Singleton.hpp"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "AssetRegistry/AssetInfo.h"
#include "RHI/Renderer.h"

namespace Sailor
{
	class RenderPipelineImporter final : public TSingleton<RenderPipelineImporter>, public IAssetInfoHandlerListener
	{
	public:
		using ByteCode = std::vector<uint8_t>;

		static SAILOR_API void Initialize();
		virtual SAILOR_API ~RenderPipelineImporter() override;

		virtual SAILOR_API void OnImportAsset(AssetInfoPtr assetInfo) override; 
		virtual SAILOR_API void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;
	};
}
