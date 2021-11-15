#pragma once
#include "Defines.h"
#include <string>
#include <vector>
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Core/Singleton.hpp"
#include "Core/SharedPtr.hpp"
#include "Core/WeakPtr.hpp"
#include "AssetInfo.h"
#include "RHI/Renderer.h"

namespace Sailor
{
	class TextureImporter final : public TSingleton<TextureImporter>, public IAssetInfoHandlerListener
	{
	public:
		using ByteCode = std::vector<uint8_t>;

		static SAILOR_API void Initialize();
		virtual SAILOR_API ~TextureImporter() override;

		virtual SAILOR_API void OnAssetInfoUpdated(AssetInfoPtr assetInfo) override;

		static SAILOR_API bool LoadTextureRaw(UID uid, ByteCode& decodedData, int32_t& width, int32_t& height, uint32_t& mipLevels);
		static SAILOR_API RHI::TexturePtr LoadTexture(UID uid);

	private:

	};
}
