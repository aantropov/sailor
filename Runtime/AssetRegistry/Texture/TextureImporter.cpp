#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/FileId.h"
#include "AssetRegistry/AssetRegistry.h"
#include "TextureAssetInfo.h"
#include "Core/Utils.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>
#include "nlohmann_json/include/nlohmann/json.hpp"
#include "Tasks/Scheduler.h"
#include "RHI/Texture.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"

#ifndef STB_IMAGE_IMPLEMENTATION

#define STB_IMAGE_IMPLEMENTATION
#define STBI_MSC_SECURE_CRT
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image.h"

#endif

using namespace Sailor;

bool Texture::IsReady() const
{
	return m_rhiTexture && m_rhiTexture->IsReady();
}

TextureImporter::TextureImporter(TextureAssetInfoHandler* infoHandler)
{
	SAILOR_PROFILE_FUNCTION();
	m_allocator = ObjectAllocatorPtr::Make(EAllocationPolicy::SharedMemory_MultiThreaded);
	infoHandler->Subscribe(this);

	auto& driver = RHI::Renderer::GetDriver();

	m_textureSamplersBindings = driver->CreateShaderBindings();

	TVector<RHI::RHITexturePtr> defaultTextures(MaxTexturesInScene);
	for (auto& t : defaultTextures)
	{
		t = driver->GetDefaultTexture();
	}

	m_textureSamplersCurrentIndex = 1;

	auto textures = driver->AddSamplerToShaderBindings(m_textureSamplersBindings, "textureSamplers", defaultTextures, 0);
	m_textureSamplersBindings->RecalculateCompatibility();
}

TextureImporter::~TextureImporter()
{
	for (auto& instance : m_loadedTextures)
	{
		instance.m_second.DestroyObject(m_allocator);
	}
}

TexturePtr TextureImporter::GetLoadedTexture(FileId uid)
{
	// Check loaded materials
	auto it = m_loadedTextures.Find(uid);
	if (it != m_loadedTextures.end())
	{
		return (*it).m_second;
	}
	return TexturePtr();
}

Tasks::TaskPtr<TexturePtr> TextureImporter::GetLoadPromise(FileId uid)
{
	auto it = m_promises.Find(uid);
	if (it != m_promises.end())
	{
		return (*it).m_second;
	}

	return Tasks::TaskPtr<TexturePtr>();
}

void TextureImporter::OnUpdateAssetInfo(AssetInfoPtr inAssetInfo, bool bWasExpired)
{
	TexturePtr pTexture = GetLoadedTexture(inAssetInfo->GetFileId());
	if (bWasExpired && pTexture)
	{
		if (TextureAssetInfoPtr assetInfo = dynamic_cast<TextureAssetInfo*>(inAssetInfo))
		{
			auto newPromise = Tasks::CreateTaskWithResult<bool>("Update Texture",
				[pTexture, assetInfo, this]() mutable
				{
					ByteCode decodedData;
					int32_t width;
					int32_t height;
					uint32_t mipLevels;

					if (ImportTexture(assetInfo->GetFileId(), decodedData, width, height, mipLevels))
					{
						pTexture->m_rhiTexture = RHI::Renderer::GetDriver()->CreateTexture(&decodedData[0], decodedData.Num(), glm::vec3(width, height, 1.0f),
							mipLevels, RHI::ETextureType::Texture2D, assetInfo->GetFormat(), assetInfo->GetFiltration(),
							assetInfo->GetClamping(),
							assetInfo->ShouldSupportStorageBinding() ? TextureImporter::DefaultTextureUsage | RHI::ETextureUsageBit::Storage_Bit : TextureImporter::DefaultTextureUsage,
							assetInfo->GetSamplerReduction());

						RHI::Renderer::GetDriver()->SetDebugName(pTexture->m_rhiTexture, assetInfo->GetAssetFilepath());

						size_t index = m_textureSamplersIndices[assetInfo->GetFileId()];
						RHI::Renderer::GetDriver()->UpdateShaderBinding(m_textureSamplersBindings, "textureSamplers", pTexture->m_rhiTexture, (uint32_t)index);
						return true;
					}
					return false;
				}, Tasks::EThreadType::RHI)->Run();

				pTexture->TraceHotReload(newPromise);
		}
	}
}

void TextureImporter::OnImportAsset(AssetInfoPtr assetInfo)
{
}

bool TextureImporter::IsTextureLoaded(FileId uid) const
{
	return m_loadedTextures.ContainsKey(uid);
}

bool TextureImporter::ImportTexture(FileId uid, ByteCode& decodedData, int32_t& width, int32_t& height, uint32_t& mipLevels)
{
	SAILOR_PROFILE_FUNCTION();

	if (TextureAssetInfoPtr assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<TextureAssetInfoPtr>(uid))
	{
		int32_t texChannels = 0;
		const std::string filepath = assetInfo->GetAssetFilepath();

		if (stbi_is_hdr(filepath.c_str()))
		{
			if (float* pixels = stbi_loadf(filepath.c_str(), &width, &height, &texChannels, STBI_rgb_alpha))
			{
				const uint32_t imageSize = (uint32_t)width * height * sizeof(float) * 4;
				decodedData.Resize(imageSize);
				memcpy(decodedData.GetData(), pixels, imageSize);

				mipLevels = assetInfo->ShouldGenerateMips() ? static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1 : 1;
				stbi_image_free(pixels);
				return true;
			}
		}
		else if (stbi_uc* pixels = stbi_load(filepath.c_str(), &width, &height, &texChannels, STBI_rgb_alpha))
		{
			const uint32_t imageSize = (uint32_t)width * height * 4;
			decodedData.Resize(imageSize);
			memcpy(decodedData.GetData(), pixels, imageSize);

			mipLevels = assetInfo->ShouldGenerateMips() ? static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1 : 1;
			stbi_image_free(pixels);
			return true;
		}
	}

	return false;
}

bool TextureImporter::LoadTexture_Immediate(FileId uid, TexturePtr& outTexture)
{
	auto task = LoadTexture(uid, outTexture);
	task->Wait();
	return task->GetResult().IsValid();
}

Tasks::TaskPtr<TexturePtr> TextureImporter::LoadTexture(FileId uid, TexturePtr& outTexture)
{
	SAILOR_PROFILE_FUNCTION();

	// Check promises first
	auto& promise = m_promises.At_Lock(uid, nullptr);
	auto& loadedTexture = m_loadedTextures.At_Lock(uid, TexturePtr());

	// Check loaded textures
	if (loadedTexture)
	{
		outTexture = loadedTexture;
		auto res = promise ? promise : Tasks::TaskPtr<TexturePtr>::Make(outTexture);

		m_loadedTextures.Unlock(uid);
		m_promises.Unlock(uid);

		return res;
	}

	if (TextureAssetInfoPtr assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<TextureAssetInfoPtr>(uid))
	{
		TexturePtr pTexture = TexturePtr::Make(m_allocator, uid);

		struct Data
		{
			ByteCode decodedData;
			int32_t width;
			int32_t height;
			uint32_t mipLevels;
			bool bIsImported;
		};

		promise = Tasks::CreateTaskWithResult<TSharedPtr<Data>>("Load Texture",
			[pTexture, assetInfo, this]() mutable
			{
				TSharedPtr<Data> pData = TSharedPtr<Data>::Make();
				pData->bIsImported = ImportTexture(assetInfo->GetFileId(), pData->decodedData, pData->width, pData->height, pData->mipLevels);

				if (!pData->bIsImported)
				{
					SAILOR_LOG("Cannot Load texture: %s, with uid: %s", assetInfo->GetAssetFilepath().c_str(), assetInfo->GetFileId().ToString().c_str());
				}

				return pData;
			})->Then<TexturePtr>([pTexture, assetInfo, this](TSharedPtr<Data> data) mutable
				{
					if (data->bIsImported && data->decodedData.Num() > 0)
					{
						pTexture->m_rhiTexture = RHI::Renderer::GetDriver()->CreateTexture(&data->decodedData[0], data->decodedData.Num(), glm::vec3(data->width, data->height, 1.0f),
							data->mipLevels, RHI::ETextureType::Texture2D, assetInfo->GetFormat(), assetInfo->GetFiltration(),
							assetInfo->GetClamping(),
							assetInfo->ShouldSupportStorageBinding() ? (TextureImporter::DefaultTextureUsage | RHI::ETextureUsageBit::Storage_Bit) : TextureImporter::DefaultTextureUsage,
							assetInfo->GetSamplerReduction());

						RHI::Renderer::GetDriver()->SetDebugName(pTexture->m_rhiTexture, assetInfo->GetAssetFilepath());

						size_t index = m_textureSamplersCurrentIndex++;
						m_textureSamplersIndices[assetInfo->GetFileId()] = index;
						RHI::Renderer::GetDriver()->UpdateShaderBinding(m_textureSamplersBindings, "textureSamplers", pTexture->m_rhiTexture, (uint32_t)index);
					}

					return pTexture;
				}, "Create RHI texture", Tasks::EThreadType::RHI)->ToTaskWithResult();

				outTexture = loadedTexture = pTexture;
				promise->Run();

				m_promises.Unlock(uid);
				m_loadedTextures.Unlock(uid);

				return promise;
	}

	outTexture = nullptr;
	m_promises.Unlock(uid);
	m_loadedTextures.Unlock(uid);

	SAILOR_LOG("Cannot find texture with uid: %s", uid.ToString().c_str());
	return Tasks::TaskPtr<TexturePtr>();
}

void TextureImporter::CollectGarbage()
{
	TVector<FileId> uidsToRemove;

	m_promises.LockAll();
	auto ids = m_promises.GetKeys();
	m_promises.UnlockAll();

	for (const auto& id : ids)
	{
		auto promise = m_promises.At_Lock(id);

		if (!promise.IsValid() || (promise.IsValid() && promise->IsFinished()))
		{
			FileId uid = id;
			uidsToRemove.Emplace(uid);
		}

		m_promises.Unlock(id);
	}

	for (auto& uid : uidsToRemove)
	{
		m_promises.Remove(uid);
	}
}
