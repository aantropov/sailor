#pragma once
#include "Core/Defines.h"
#include <string>
#include <vector>
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Core/Submodule.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "AssetRegistry/AssetInfo.h"
#include "TextureAssetInfo.h"
#include "RHI/Renderer.h"

namespace Sailor
{
	class TextureImporter final : public TSubmodule<TextureImporter>, public IAssetInfoHandlerListener
	{
	public:
		using ByteCode = std::vector<uint8_t>;

		SAILOR_API TextureImporter(TextureAssetInfoHandler* infoHandler);
		virtual SAILOR_API ~TextureImporter() override;

		virtual SAILOR_API void OnImportAsset(AssetInfoPtr assetInfo) override; 
		virtual SAILOR_API void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;

		static SAILOR_API bool LoadTextureRaw(UID uid, ByteCode& decodedData, int32_t& width, int32_t& height, uint32_t& mipLevels);
		static SAILOR_API RHI::TexturePtr LoadTexture(UID uid);

	private:

	};
}
