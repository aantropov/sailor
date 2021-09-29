#include "TextureImporter.h"

#include "UID.h"
#include "AssetRegistry.h"
#include "TextureAssetInfo.h"
#include "Utils.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>

#include "nlohmann_json/include/nlohmann/json.hpp"
#include "JobSystem/JobSystem.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

using namespace Sailor;

void TextureImporter::Initialize()
{
	SAILOR_PROFILE_FUNCTION();

	m_pInstance = new TextureImporter();
	TextureAssetInfoHandler::GetInstance()->Subscribe(m_pInstance);
}

TextureImporter::~TextureImporter()
{
}

void TextureImporter::OnAssetInfoUpdated(AssetInfo* assetInfo)
{
}

bool TextureImporter::LoadTexture(UID uid, ByteCode& decodedData, int32_t& width, int32_t& height)
{
	SAILOR_PROFILE_FUNCTION();

	if (TextureAssetInfo* assetInfo = AssetRegistry::GetInstance()->GetAssetInfo<TextureAssetInfo>(uid))
	{
		int32_t texChannels = 0;

		stbi_uc* pixels = stbi_load(assetInfo->GetAssetFilepath().c_str(), &width, &height, &texChannels, STBI_rgb_alpha);
		uint32_t imageSize = (uint32_t)width * height * 4;
		decodedData.resize(imageSize);
		memcpy(decodedData.data(), pixels, imageSize);

		stbi_image_free(pixels);
		return true;
	}

	return false;
}
