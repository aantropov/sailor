#include "AssetRegistry/Texture/TextureImporter.h"
#include "Containers/Containers.h"
#include "AssetRegistry/FileId.h"
#include "AssetRegistry/AssetRegistry.h"
#include "TextureAssetInfo.h"
#include "Core/Utils.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstring>
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

	TVector<char> jsonChunk(jsonChunkHeader.chunkLength);
	file.read(jsonChunk.GetData(), jsonChunkHeader.chunkLength);
	const nlohmann::json gltfJson = nlohmann::json::parse(
		jsonChunk.GetData(),
		jsonChunk.GetData() + jsonChunk.Num(),
		nullptr,
		false);

	if (gltfJson.is_discarded() || !gltfJson.is_object())
	{
		SAILOR_LOG_ERROR("Failed to extract texture from GLB, Invalid JSON: %s", filePath.c_str());
		return false;
	}

	auto tryGetNonNegativeInteger = [](const nlohmann::json& value, uint64_t& outValue)
		{
			if (value.is_number_unsigned())
			{
				outValue = value.get<uint64_t>();
				return true;
			}

			if (value.is_number_integer())
			{
				const int64_t signedValue = value.get<int64_t>();
				if (signedValue >= 0)
				{
					outValue = static_cast<uint64_t>(signedValue);
					return true;
				}
			}

			return false;
		};

	const auto texturesIt = gltfJson.find("textures");
	const auto imagesIt = gltfJson.find("images");
	const auto bufferViewsIt = gltfJson.find("bufferViews");
	if (texturesIt == gltfJson.end() || !texturesIt->is_array() ||
		imagesIt == gltfJson.end() || !imagesIt->is_array() ||
		bufferViewsIt == gltfJson.end() || !bufferViewsIt->is_array() ||
		textureIndex < 0 || static_cast<size_t>(textureIndex) >= texturesIt->size())
	{
		SAILOR_LOG_ERROR("Failed to extract texture from GLB, Invalid texture index %d: %s", textureIndex, filePath.c_str());
		return false;
	}

	const auto& texture = (*texturesIt)[static_cast<size_t>(textureIndex)];
	const auto sourceIt = texture.is_object() ? texture.find("source") : texture.end();
	uint64_t imageIndex = 0;
	if (!texture.is_object() || sourceIt == texture.end() ||
		!tryGetNonNegativeInteger(*sourceIt, imageIndex) || imageIndex >= imagesIt->size())
	{
		SAILOR_LOG_ERROR("Failed to extract texture from GLB, Invalid image source for texture %d: %s", textureIndex, filePath.c_str());
		return false;
	}

	const auto& image = (*imagesIt)[static_cast<size_t>(imageIndex)];
	const auto bufferViewIt = image.is_object() ? image.find("bufferView") : image.end();
	uint64_t bufferViewIndex = 0;
	if (!image.is_object() || bufferViewIt == image.end() ||
		!tryGetNonNegativeInteger(*bufferViewIt, bufferViewIndex) || bufferViewIndex >= bufferViewsIt->size())
	{
		SAILOR_LOG_ERROR("Failed to extract texture from GLB, Invalid image buffer view for texture %d: %s", textureIndex, filePath.c_str());
		return false;
	}

	const auto& bufferView = (*bufferViewsIt)[static_cast<size_t>(bufferViewIndex)];
	if (!bufferView.is_object())
	{
		SAILOR_LOG_ERROR("Failed to extract texture from GLB, Invalid buffer view for texture %d: %s", textureIndex, filePath.c_str());
		return false;
	}

	uint64_t byteOffset = 0;
	const auto byteOffsetIt = bufferView.find("byteOffset");
	if (byteOffsetIt != bufferView.end() && !tryGetNonNegativeInteger(*byteOffsetIt, byteOffset))
	{
		SAILOR_LOG_ERROR("Failed to extract texture from GLB, Invalid byte offset for texture %d: %s", textureIndex, filePath.c_str());
		return false;
	}

	uint64_t byteLength = 0;
	const auto byteLengthIt = bufferView.find("byteLength");
	if (byteLengthIt == bufferView.end() || !tryGetNonNegativeInteger(*byteLengthIt, byteLength) || byteLength == 0)
	{
		SAILOR_LOG_ERROR("Failed to extract texture from GLB, Invalid byte length for texture %d: %s", textureIndex, filePath.c_str());
		return false;
	}

	file.seekg(sizeof(GLBHeader) + sizeof(GLBChunkHeader) + jsonChunkHeader.chunkLength, std::ios::beg);

	GLBChunkHeader binChunkHeader;
	file.read(reinterpret_cast<char*>(&binChunkHeader), sizeof(GLBChunkHeader));
	if (binChunkHeader.chunkType != 0x004E4942)
	{
		SAILOR_LOG_ERROR("Failed to extract texture from GLB, Invalid BIN chunk");
		return false;
	}

	if (byteOffset > binChunkHeader.chunkLength || byteLength > binChunkHeader.chunkLength - byteOffset)
	{
		SAILOR_LOG_ERROR("Failed to extract texture from GLB, Invalid buffer view range for texture %d: %s", textureIndex, filePath.c_str());
		return false;
	}

	file.seekg(static_cast<std::streamoff>(byteOffset), std::ios::cur);
	outTexture.Resize(static_cast<size_t>(byteLength));
	file.read(reinterpret_cast<char*>(&outTexture[0]), static_cast<std::streamsize>(byteLength));
	if (!file)
	{
		outTexture.Clear();
		SAILOR_LOG_ERROR("Failed to extract texture from GLB, Cannot read texture %d: %s", textureIndex, filePath.c_str());
		return false;
	}

	return true;
}

namespace
{
	int32_t ResolveGltfTextureImageIndex(const tinygltf::Texture& texture)
	{
		if (texture.source >= 0)
		{
			return texture.source;
		}

		// Some texture formats keep their image source in an extension instead of
		// the core texture.source field. Extraction remains format-agnostic here;
		// stb_image decides below whether the encoded bytes can be decoded.
		constexpr const char* ImageSourceExtensions[] = {
			"KHR_texture_basisu",
			"EXT_texture_webp",
			"MSFT_texture_dds"
		};
		for (const char* extensionName : ImageSourceExtensions)
		{
			const auto extensionIt = texture.extensions.find(extensionName);
			if (extensionIt == texture.extensions.end() ||
				!extensionIt->second.IsObject() ||
				!extensionIt->second.Has("source"))
			{
				continue;
			}

			const tinygltf::Value& source = extensionIt->second.Get("source");
			if (source.IsInt())
			{
				return source.GetNumberAsInt();
			}
		}

		return -1;
	}

	bool ExtractTextureFromGltf(
		const std::string& filePath,
		int32_t textureIndex,
		Sailor::TextureImporter::ByteCode& outTexture,
		std::string& outDiagnostic)
	{
		outTexture.Clear();
		outDiagnostic.clear();

		tinygltf::TinyGLTF loader;
		loader.SetImagesAsIs(true);

		tinygltf::Model gltfModel;
		std::string error;
		std::string warning;
		if (!loader.LoadASCIIFromFile(
				&gltfModel,
				&error,
				&warning,
				filePath.c_str()))
		{
			outDiagnostic = !error.empty() ? error : warning;
			if (outDiagnostic.empty())
			{
				outDiagnostic = "tinygltf could not load the source document";
			}
			return false;
		}

		if (textureIndex < 0 ||
			static_cast<size_t>(textureIndex) >= gltfModel.textures.size())
		{
			outDiagnostic = "texture index is outside the glTF textures array";
			return false;
		}

		const int32_t imageIndex = ResolveGltfTextureImageIndex(
			gltfModel.textures[static_cast<size_t>(textureIndex)]);
		if (imageIndex < 0 ||
			static_cast<size_t>(imageIndex) >= gltfModel.images.size())
		{
			outDiagnostic = "texture does not reference a valid glTF image";
			return false;
		}

		const tinygltf::Image& image =
			gltfModel.images[static_cast<size_t>(imageIndex)];
		if (image.image.empty())
		{
			outDiagnostic = warning.empty() ?
				"referenced glTF image contains no encoded bytes" :
				warning;
			return false;
		}

		outTexture.Resize(image.image.size());
		memcpy(
			outTexture.GetData(),
			image.image.data(),
			image.image.size());
		return true;
	}
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

	TVector<RHI::RHITexturePtr> defaultTextures(1);
	defaultTextures[0] = driver->GetDefaultTexture();

	m_textureSamplersCurrentIndex = 1;

	auto textures = driver->AddSamplerToShaderBindings(m_textureSamplersBindings, "textureSamplers", defaultTextures, 0, true, static_cast<uint32_t>(MaxTexturesInScene));
	m_textureSamplersBindings->RecalculateCompatibility();

	m_textureSamplerSlotRevisions.Resize(1);
	m_textureSamplerSlotRevisions[0] = m_textureSamplersBindings->GetDescriptorRevision();
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

TextureImporter::TextureSamplersSnapshot TextureImporter::GetTextureSamplersSnapshot(const TVector<uint32_t>& requestedIndices) const
{
	TextureSamplersSnapshot snapshot;
	snapshot.m_slots.Reserve(requestedIndices.Num());
	m_textureSamplersLock.Lock();

	if (m_textureSamplersBindings)
	{
		const auto& shaderBindings = m_textureSamplersBindings->GetShaderBindings();
		const auto textureSamplers = shaderBindings.Find("textureSamplers");
		const TVector<RHI::RHITexturePtr>* textures = nullptr;
		if (textureSamplers != shaderBindings.end() && textureSamplers->m_second)
		{
			textures = &textureSamplers->m_second->GetTextureBindings();
		}

		for (const uint32_t requestedIndex : requestedIndices)
		{
			TextureSamplerSlotSnapshot slot;
			slot.m_index = requestedIndex;
			if (requestedIndex < m_textureSamplerSlotRevisions.Num())
			{
				slot.m_contentRevision = m_textureSamplerSlotRevisions[requestedIndex];
			}

			if (textures && requestedIndex < textures->Num())
			{
				slot.m_texture = (*textures)[requestedIndex];
			}

			snapshot.m_slots.Emplace(std::move(slot));
		}

		snapshot.m_descriptorRevision = m_textureSamplersBindings->GetDescriptorRevision();
	}

	m_textureSamplersLock.Unlock();
	return snapshot;
}

bool TextureImporter::RegisterTextureSamplerBinding(RHI::RHITexturePtr texture, size_t& outIndex)
{
	outIndex = 0;
	if (!texture)
	{
		return false;
	}

	m_textureSamplersLock.Lock();
	const size_t nextIndex = m_textureSamplersCurrentIndex.load(std::memory_order_relaxed);
	const bool bCanRegister = IsUserTextureSamplerIndexValid(nextIndex);
	bool bRegistered = false;
	if (bCanRegister)
	{
		outIndex = nextIndex;
		bRegistered = UpdateTextureSamplerBindingLocked(
			texture,
			static_cast<uint32_t>(nextIndex));
		if (bRegistered)
		{
			// Publish the next free slot only after the native descriptor write and slot
			// revision have both succeeded. A failed write can therefore be retried.
			m_textureSamplersCurrentIndex.store(nextIndex + 1, std::memory_order_release);
		}
	}

	m_textureSamplersLock.Unlock();
	return bRegistered;
}

bool TextureImporter::UpdateTextureSamplerBinding(RHI::RHITexturePtr texture, uint32_t index)
{
	if (!IsUserTextureSamplerIndexValid(index) || !texture)
	{
		return false;
	}

	m_textureSamplersLock.Lock();
	const bool bUpdated = UpdateTextureSamplerBindingLocked(std::move(texture), index);
	m_textureSamplersLock.Unlock();
	return bUpdated;
}

bool TextureImporter::UpdateTextureSamplerBindingLocked(RHI::RHITexturePtr texture, uint32_t index)
{
	const uint64_t previousRevision = m_textureSamplersBindings->GetDescriptorRevision();
	RHI::Renderer::GetDriver()->UpdateShaderBinding(m_textureSamplersBindings, "textureSamplers", texture, index);
	const uint64_t currentRevision = m_textureSamplersBindings->GetDescriptorRevision();

	if (currentRevision == previousRevision)
	{
		return false;
	}

	if (m_textureSamplerSlotRevisions.Num() <= index)
	{
		m_textureSamplerSlotRevisions.Resize(index + 1);
	}
	m_textureSamplerSlotRevisions[index] = currentRevision;
	return true;
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

						return UpdateTextureSamplerBinding(pTexture->m_rhiTexture, static_cast<uint32_t>(index));
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
			const std::string extension =
				Utils::GetFileExtension(assetInfo->GetAssetFilepath().c_str());
			const bool bIsGlb = extension == "glb";
			const bool bIsGltf = extension == "gltf";

			if ((!bIsGlb && !bIsGltf) ||
				assetInfo->GetGlbTextureIndex() == -1)
			{
				return false;
			}

			ByteCode rawBuffer;
			std::string extractionDiagnostic;
			const bool bExtracted = bIsGlb ?
				ExtractTextureFromGLB(
					assetInfo->GetAssetFilepath().c_str(),
					assetInfo->GetGlbTextureIndex(),
					rawBuffer) :
				ExtractTextureFromGltf(
					assetInfo->GetAssetFilepath(),
					assetInfo->GetGlbTextureIndex(),
					rawBuffer,
					extractionDiagnostic);
			if (!bExtracted && extractionDiagnostic.empty())
			{
				extractionDiagnostic = bIsGlb ?
					"GLB extraction failed" :
					"glTF extraction failed";
			}

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
				SAILOR_LOG_ERROR(
					"Cannot extract texture %d from model source '%s' for asset %s: %s",
					assetInfo->GetGlbTextureIndex(),
					assetInfo->GetAssetFilepath().c_str(),
					uid.ToString().c_str(),
					extractionDiagnostic.c_str());
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

bool TextureImporter::DecodeTextureCpu(FileId uid, ByteCode& decodedData,
	int32_t& width, int32_t& height, uint32_t& mipLevels)
{
	return ImportTexture(uid, decodedData, width, height, mipLevels);
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

						size_t index = 0;
						if (RegisterTextureSamplerBinding(pTexture->m_rhiTexture, index))
						{
							m_textureSamplersIndices.At_Lock(pAssetInfo->GetFileId()) = index;
							m_textureSamplersIndices.Unlock(pAssetInfo->GetFileId());
						}
						else if (index == 0)
						{
							SAILOR_LOG_ERROR("Cannot register texture sampler '%s': the scene texture capacity of %zu user textures is exhausted.",
								pAssetInfo->GetAssetFilepath().c_str(),
								MaxUserTexturesInScene);
						}
						else
						{
							SAILOR_LOG_ERROR("Cannot register texture sampler '%s' at index %zu: descriptor update failed.",
								pAssetInfo->GetAssetFilepath().c_str(),
								index);
						}
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
