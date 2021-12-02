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

bool Texture::IsReady() const
{
	return m_rhiTexture && m_rhiTexture->IsReady();
}

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

bool TextureImporter::IsTextureLoaded(UID uid) const
{
	return m_loadedTextures.find(uid) != m_loadedTextures.end();
}

bool TextureImporter::ImportTexture(UID uid, ByteCode& decodedData, int32_t& width, int32_t& height, uint32_t& mipLevels)
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

bool TextureImporter::LoadTexture_Immediate(UID uid, TexturePtr& outTexture)
{
	SAILOR_PROFILE_FUNCTION();

	auto it = m_loadedTextures.find(uid);
	if (it != m_loadedTextures.end())
	{
		return outTexture = (*it).second;
	}

	outTexture = nullptr;

	if (TextureAssetInfoPtr assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<TextureAssetInfoPtr>(uid))
	{
		ByteCode decodedData;
		int32_t width;
		int32_t height;
		uint32_t mipLevels;

		if (ImportTexture(uid, decodedData, width, height, mipLevels))
		{
			auto pTexture = TSharedPtr<Texture>::Make(uid);

			pTexture->m_rhiTexture = RHI::Renderer::GetDriver()->CreateImage(&decodedData[0], decodedData.size(), glm::vec3(width, height, 1.0f),
				mipLevels, RHI::ETextureType::Texture2D, RHI::ETextureFormat::R8G8B8A8_SRGB, assetInfo->GetFiltration(),
				assetInfo->GetClamping());

			{
				std::scoped_lock<std::mutex> guard(m_mutex);
				return outTexture = m_loadedTextures[uid] = pTexture;
			}
		}

		SAILOR_LOG("Cannot import texture with uid: %s", uid.ToString().c_str());

		return false;
	}

	SAILOR_LOG("Cannot find texture with uid: %s", uid.ToString().c_str());
	return false;
}

JobSystem::TaskPtr<bool> TextureImporter::LoadTexture(UID uid, TexturePtr& outTexture)
{
	SAILOR_PROFILE_FUNCTION();

	auto it = m_loadedTextures.find(uid);
	if (it != m_loadedTextures.end())
	{
		outTexture = (*it).second;
		return JobSystem::TaskPtr<bool>::Make(true);
	}

	JobSystem::TaskPtr<bool> outLoadingTask;

	outTexture = nullptr;

	if (TextureAssetInfoPtr assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<TextureAssetInfoPtr>(uid))
	{
		auto pTexture = TSharedPtr<Texture>::Make(uid);

		outLoadingTask = JobSystem::Scheduler::CreateTaskWithResult<bool>("Load model",
			[pTexture, assetInfo, this]()
			{
				ByteCode decodedData;
				int32_t width;
				int32_t height;
				uint32_t mipLevels;

				if (ImportTexture(assetInfo->GetUID(), decodedData, width, height, mipLevels))
				{
					pTexture.GetRawPtr()->m_rhiTexture = RHI::Renderer::GetDriver()->CreateImage(&decodedData[0], decodedData.size(), glm::vec3(width, height, 1.0f),
						mipLevels, RHI::ETextureType::Texture2D, RHI::ETextureFormat::R8G8B8A8_SRGB, assetInfo->GetFiltration(),
						assetInfo->GetClamping());
					return true;
				}
				return false;
			});

		App::GetSubmodule<JobSystem::Scheduler>()->Run(outLoadingTask);

		{
			std::scoped_lock<std::mutex> guard(m_mutex);
			outTexture = m_loadedTextures[uid] = pTexture;
		}
		return outLoadingTask;
	}

	SAILOR_LOG("Cannot find texture with uid: %s", uid.ToString().c_str());
	return JobSystem::TaskPtr<bool>::Make(false);
}