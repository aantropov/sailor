#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/FileId.h"
#include "AssetRegistry/AssetRegistry.h"
#include "TextureAssetInfo.h"
#include "Core/Utils.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>
#include "Tasks/Scheduler.h"
#include "RHI/Texture.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"

#include <tiny_gltf.h>

#ifndef STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STBI_MSC_SECURE_CRT
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#endif

using namespace Sailor;

bool ExtractTextureFromGLB(const std::string& filePath, int32_t textureIndex, Sailor::TextureImporter::ByteCode& outTexture)
{
	struct GLBHeader
	{
		uint32_t magic;
		uint32_t version;
		uint32_t length;
	};

	struct GLBChunkHeader
	{
		uint32_t chunkLength;
		uint32_t chunkType;
	};

	std::ifstream file(filePath, std::ios::binary);
	if (!file.is_open())
	{
		SAILOR_LOG_ERROR("Failed to open file");
		return false;
	}

	GLBHeader header;
	file.read(reinterpret_cast<char*>(&header), sizeof(GLBHeader));
	if (header.magic != 0x46546C67)
	{
		SAILOR_LOG_ERROR("Failed to extract texture from GLB, Invalid GLB magic");
		return false;
	}

	GLBChunkHeader jsonChunkHeader;
	file.read(reinterpret_cast<char*>(&jsonChunkHeader), sizeof(GLBChunkHeader));
	if (jsonChunkHeader.chunkType != 0x4E4F534A)
	{
		SAILOR_LOG_ERROR("Failed to extract texture from GLB, Invalid JSON chunk");
		return false;
	}

	std::vector<char> jsonChunk(jsonChunkHeader.chunkLength);
	file.read(jsonChunk.data(), jsonChunkHeader.chunkLength);
	nlohmann::json gltfJson = nlohmann::json::parse(jsonChunk);

	if (textureIndex < 0 || textureIndex >= gltfJson["textures"].size())
	{
		SAILOR_LOG_ERROR("Failed to extract texture from GLB, Invalid texture index");
		return false;
	}

	int32_t imageIndex = gltfJson["textures"][textureIndex]["source"];
	int32_t bufferViewIndex = gltfJson["images"][imageIndex]["bufferView"];
	const auto& bufferView = gltfJson["bufferViews"][bufferViewIndex];
	//int32_t bufferIndex = bufferView["buffer"];
	int32_t byteOffset = bufferView["byteOffset"];
	int32_t byteLength = bufferView["byteLength"];

	file.seekg(sizeof(GLBHeader) + sizeof(GLBChunkHeader) + jsonChunkHeader.chunkLength, std::ios::beg);

	GLBChunkHeader binChunkHeader;
	file.read(reinterpret_cast<char*>(&binChunkHeader), sizeof(GLBChunkHeader));
	if (binChunkHeader.chunkType != 0x004E4942)
	{
		SAILOR_LOG_ERROR("Failed to extract texture from GLB, Invalid BIN chunk");
		return false;
	}

	if (byteOffset + byteLength > (int32_t)binChunkHeader.chunkLength)
	{
		SAILOR_LOG_ERROR("Failed to extract texture from GLB, Invalid buffer view");
		return false;
	}

	file.seekg(byteOffset, std::ios::cur);
	outTexture.Resize(byteLength);
	file.read(reinterpret_cast<char*>(&outTexture[0]), byteLength);

	return true;
}

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

	// Extremely large descriptor arrays (MaxTexturesInScene) can exceed runtime Vulkan limits on macOS/MoltenVK
	// and crash during vkUpdateDescriptorSets. Start with a conservative bindless table size.
	constexpr size_t kMaxInitialTextureSamplers = 1024;
	const size_t initialTextureSamplers = std::min(MaxTexturesInScene, kMaxInitialTextureSamplers);

	TVector<RHI::RHITexturePtr> defaultTextures(initialTextureSamplers);
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
	SAILOR_PROFILE_FUNCTION();
	SAILOR_PROFILE_TEXT(inAssetInfo->GetAssetFilepath().c_str());

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
						pTexture->m_width = width;
						pTexture->m_height = height;
						pTexture->m_mipLevels = mipLevels;
						if (assetInfo->ShouldKeepCpuBuffers())
						{
							pTexture->m_decodedData = std::move(decodedData);
						}
						else
						{
							pTexture->m_decodedData.Clear();
						}

						RHI::Renderer::GetDriver()->SetDebugName(pTexture->m_rhiTexture, assetInfo->GetAssetFilepath());

						size_t index = m_textureSamplersIndices.At_Lock(assetInfo->GetFileId());
						m_textureSamplersIndices.Unlock(assetInfo->GetFileId());

						RHI::Renderer::GetDriver()->UpdateShaderBinding(m_textureSamplersBindings, "textureSamplers", pTexture->m_rhiTexture, (uint32_t)index);
						return true;
					}
					return false;
				}, EThreadType::RHI)->Run();

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
		const bool bDecodeAsFloat = RHI::IsFloatFormat(assetInfo->GetFormat());

		if (assetInfo->StoredInGlb())
		{
			const bool bIsGlb = Utils::GetFileExtension(assetInfo->GetAssetFilepath().c_str()) == "glb";

			if (!bIsGlb || (assetInfo->GetGlbTextureIndex() == -1))
			{
				return false;
			}

			ByteCode rawBuffer;
			const bool bExtracted = ExtractTextureFromGLB(assetInfo->GetAssetFilepath().c_str(), assetInfo->GetGlbTextureIndex(), rawBuffer);

			if (bExtracted)
			{
				int32_t texChannels = 0;
				const std::string filepath = assetInfo->GetAssetFilepath();

				if (bDecodeAsFloat)
				{
					if (float* pPixels = stbi_loadf_from_memory(&rawBuffer[0], (uint32_t)rawBuffer.Num(), &width, &height, &texChannels, STBI_rgb_alpha))
					{
						const uint32_t imageSize = (uint32_t)width * height * sizeof(float) * 4;
						decodedData.Resize(imageSize);
						memcpy(decodedData.GetData(), pPixels, imageSize);

						mipLevels = assetInfo->ShouldGenerateMips() ? static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1 : 1;
						stbi_image_free(pPixels);
						return true;
					}
				}
				else if (stbi_uc* pPixels = stbi_load_from_memory(&rawBuffer[0], (uint32_t)rawBuffer.Num(), &width, &height, &texChannels, STBI_rgb_alpha))
				{
					const uint32_t imageSize = (uint32_t)width * height * 4;
					decodedData.Resize(imageSize);
					memcpy(decodedData.GetData(), pPixels, imageSize);

					mipLevels = assetInfo->ShouldGenerateMips() ? static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1 : 1;
					stbi_image_free(pPixels);
					return true;
				}
			}
			else
			{
				const auto msg = "Cannot extract texture from glb! " + uid.ToString();
				SAILOR_LOG_ERROR("%s", msg.c_str());
			}

			return false;
		}
		else
		{
			int32_t texChannels = 0;
			const std::string filepath = assetInfo->GetAssetFilepath();

			if (bDecodeAsFloat)
			{
				if (float* pPixels = stbi_loadf(filepath.c_str(), &width, &height, &texChannels, STBI_rgb_alpha))
				{
					const uint32_t imageSize = (uint32_t)width * height * sizeof(float) * 4;
					decodedData.Resize(imageSize);
					memcpy(decodedData.GetData(), pPixels, imageSize);

					mipLevels = assetInfo->ShouldGenerateMips() ? static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1 : 1;
					stbi_image_free(pPixels);
					return true;
				}
			}
			else if (stbi_uc* pPixels = stbi_load(filepath.c_str(), &width, &height, &texChannels, STBI_rgb_alpha))
			{
				const uint32_t imageSize = (uint32_t)width * height * 4;
				decodedData.Resize(imageSize);
				memcpy(decodedData.GetData(), pPixels, imageSize);

				mipLevels = assetInfo->ShouldGenerateMips() ? static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1 : 1;
				stbi_image_free(pPixels);
				return true;
			}
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
	TextureAssetInfoPtr pAssetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<TextureAssetInfoPtr>(uid);

	// Check promises first
	auto& promise = m_promises.At_Lock(uid, nullptr);
	auto& loadedTexture = m_loadedTextures.At_Lock(uid, TexturePtr());

	// Check loaded textures
	if (loadedTexture)
	{
		const bool bNeedCpuBuffers = pAssetInfo && pAssetInfo->ShouldKeepCpuBuffers() && !loadedTexture->HasCpuData();
		if (bNeedCpuBuffers && !promise)
		{
			loadedTexture = nullptr;
		}
		else
		{
			outTexture = loadedTexture;
			auto res = promise ? promise : Tasks::TaskPtr<TexturePtr>::Make(outTexture);

			m_loadedTextures.Unlock(uid);
			m_promises.Unlock(uid);

			return res;
		}
	}

	if (pAssetInfo)
	{
		SAILOR_PROFILE_TEXT(pAssetInfo->GetAssetFilepath().c_str());

		TexturePtr pTexture = TexturePtr::Make(m_allocator, uid);

		struct Data
		{
			ByteCode decodedData;
			int32_t width;
			int32_t height;
			uint32_t mipLevels;
			bool bIsImported;
			bool bShouldKeepCpuBuffers;
		};

		promise = Tasks::CreateTaskWithResult<TSharedPtr<Data>>("Load Texture",
			[pAssetInfo]() mutable
			{
				TSharedPtr<Data> pData = TSharedPtr<Data>::Make();
				pData->bIsImported = ImportTexture(pAssetInfo->GetFileId(), pData->decodedData, pData->width, pData->height, pData->mipLevels);
				pData->bShouldKeepCpuBuffers = pAssetInfo->ShouldKeepCpuBuffers();

				if (!pData->bIsImported)
				{
					SAILOR_LOG("Cannot Load texture: %s, with uid: %s", pAssetInfo->GetAssetFilepath().c_str(), pAssetInfo->GetFileId().ToString().c_str());
				}

				return pData;
			})->Then<TexturePtr>([pTexture, pAssetInfo, this](TSharedPtr<Data> pData) mutable
				{
					if (pData->bIsImported && pData->decodedData.Num() > 0)
					{
						pTexture->m_rhiTexture = RHI::Renderer::GetDriver()->CreateTexture(&pData->decodedData[0], pData->decodedData.Num(), glm::vec3(pData->width, pData->height, 1.0f),
							pData->mipLevels, RHI::ETextureType::Texture2D, pAssetInfo->GetFormat(), pAssetInfo->GetFiltration(),
							pAssetInfo->GetClamping(),
							pAssetInfo->ShouldSupportStorageBinding() ? (TextureImporter::DefaultTextureUsage | RHI::ETextureUsageBit::Storage_Bit) : TextureImporter::DefaultTextureUsage,
							pAssetInfo->GetSamplerReduction());
						pTexture->m_width = pData->width;
						pTexture->m_height = pData->height;
						pTexture->m_mipLevels = pData->mipLevels;
						if (pData->bShouldKeepCpuBuffers)
						{
							pTexture->m_decodedData = std::move(pData->decodedData);
						}
						else
						{
							pTexture->m_decodedData.Clear();
						}

						RHI::Renderer::GetDriver()->SetDebugName(pTexture->m_rhiTexture, pAssetInfo->GetAssetFilepath());

						size_t index = m_textureSamplersCurrentIndex++;

						m_textureSamplersIndices.At_Lock(pAssetInfo->GetFileId()) = index;
						m_textureSamplersIndices.Unlock(pAssetInfo->GetFileId());

						RHI::Renderer::GetDriver()->UpdateShaderBinding(m_textureSamplersBindings, "textureSamplers", pTexture->m_rhiTexture, (uint32_t)index);
					}

					return pTexture;
				}, "Create RHI texture", EThreadType::RHI)->ToTaskWithResult();

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

TexturePtr TextureImporter::Create(const ByteCode& decodedData,
	int32_t width,
	int32_t height,
	uint32_t mipLevels,
	RHI::ETextureFormat format,
	RHI::ETextureFiltration filtration,
	RHI::ETextureClamping clamping,
	RHI::ESamplerReductionMode samplerReduction,
	bool bSupportStorageBinding,
	bool bKeepCpuBuffers)
{
	SAILOR_PROFILE_FUNCTION();

	if (decodedData.Num() == 0 || width <= 0 || height <= 0)
	{
		return TexturePtr();
	}

	const FileId uid = FileId::CreateNewFileId();
	TexturePtr pTexture = TexturePtr::Make(m_allocator, uid);

	const RHI::ETextureUsageFlags usage = bSupportStorageBinding ?
		(TextureImporter::DefaultTextureUsage | RHI::ETextureUsageBit::Storage_Bit) :
		TextureImporter::DefaultTextureUsage;

	pTexture->m_rhiTexture = RHI::Renderer::GetDriver()->CreateTexture(&decodedData[0], decodedData.Num(), glm::vec3(width, height, 1.0f),
		mipLevels, RHI::ETextureType::Texture2D, format, filtration, clamping, usage, samplerReduction);
	pTexture->m_width = width;
	pTexture->m_height = height;
	pTexture->m_mipLevels = mipLevels;

	if (bKeepCpuBuffers)
	{
		pTexture->m_decodedData = decodedData;
	}
	else
	{
		pTexture->m_decodedData.Clear();
	}

	size_t index = m_textureSamplersCurrentIndex++;

	m_textureSamplersIndices.At_Lock(uid) = index;
	m_textureSamplersIndices.Unlock(uid);

	m_loadedTextures.At_Lock(uid) = pTexture;
	m_loadedTextures.Unlock(uid);

	RHI::Renderer::GetDriver()->UpdateShaderBinding(m_textureSamplersBindings, "textureSamplers", pTexture->m_rhiTexture, (uint32_t)index);
	return pTexture;
}

size_t TextureImporter::GetTextureIndex(FileId uid)
{
	size_t res = m_textureSamplersIndices.At_Lock(uid);
	m_textureSamplersIndices.Unlock(uid);

	return res;
}

bool TextureImporter::LoadAsset(FileId uid, TObjectPtr<Object>& out, bool bImmediate)
{
	TexturePtr outAsset;
	if (bImmediate)
	{
		bool bRes = LoadTexture_Immediate(uid, outAsset);
		out = outAsset;
		return bRes;
	}

	LoadTexture(uid, outAsset);
	out = outAsset;

	return true;
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


