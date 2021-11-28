#include "AssetRegistry/Texture/TextureImporter.h"

#include "AssetRegistry/UID.h"
#include "AssetRegistry/AssetRegistry.h"
#include "TextureAssetInfo.h"
#include "Core/Utils.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>

#include "nlohmann_json/include/nlohmann/json.hpp"
#include "JobSystem/JobSystem.h"
#include "RHI/Texture.h"
#include "RHI/Renderer.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

using namespace Sailor;

TextureImporter::TextureImporter(TextureAssetInfoHandler* infoHandler)
{
	SAILOR_PROFILE_FUNCTION();

	infoHandler->Subscribe(this);
}

TextureImporter::~TextureImporter()
{
}

void TextureImporter::OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired)
{
}


void TextureImporter::OnImportAsset(AssetInfoPtr assetInfo)
{
}

bool TextureImporter::LoadTextureRaw(UID uid, ByteCode& decodedData, int32_t& width, int32_t& height, uint32_t& mipLevels)
{
	SAILOR_PROFILE_FUNCTION();

	if (TextureAssetInfoPtr assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<TextureAssetInfoPtr>(uid))
	{
		int32_t texChannels = 0;

		stbi_uc* pixels = stbi_load(assetInfo->GetAssetFilepath().c_str(), &width, &height, &texChannels, STBI_rgb_alpha);
		uint32_t imageSize = (uint32_t)width * height * 4;
		decodedData.resize(imageSize);
		memcpy(decodedData.data(), pixels, imageSize);

		mipLevels = assetInfo->ShouldGenerateMips() ? static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1 : 1;
		stbi_image_free(pixels);
		return true;
	}

	return false;
}

RHI::TexturePtr TextureImporter::LoadTexture(UID uid)
{
	SAILOR_PROFILE_FUNCTION();

	if (TextureAssetInfoPtr assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<TextureAssetInfoPtr>(uid))
	{
		ByteCode decodedData;
		int32_t width;
		int32_t height;
		uint32_t mipLevels;

		if (LoadTextureRaw(uid, decodedData, width, height, mipLevels))
		{
			return RHI::Renderer::GetDriver()->CreateImage(&decodedData[0], decodedData.size(), glm::vec3(width, height, 1.0f),
				mipLevels, RHI::ETextureType::Texture2D, RHI::ETextureFormat::R8G8B8A8_SRGB, assetInfo->GetFiltration(),
				assetInfo->GetClamping());
		}
	}

	SAILOR_LOG("Cannot find texture with uid: %s", uid.ToString().c_str());
	return nullptr;
}