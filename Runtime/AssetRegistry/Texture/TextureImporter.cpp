#include "AssetRegistry/Texture/TextureImporter.h"
#include "Core/FileRevision.h"
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
	TextureImporter::CpuDecodeRequest DescribeCpuTexture(const TextureAssetInfo& assetInfo)
	{
		TextureImporter::CpuDecodeRequest request;
		request.m_fileId = assetInfo.GetFileId();
		request.m_filepath = assetInfo.GetAssetFilepath();
		request.m_glbTextureIndex = assetInfo.GetGlbTextureIndex();
		request.m_bDecodeAsFloat = RHI::IsFloatFormat(assetInfo.GetFormat());
		request.m_bGenerateMips = assetInfo.ShouldGenerateMips();
		return request;
	}

	bool HasCurrentTextureSources(const TextureImporter::CpuDecodeRequest& request)
	{
		if (request.m_sourceRevisions.IsEmpty())
		{
			return false;
		}
		for (const auto& source : request.m_sourceRevisions)
		{
			FileRevision current;
			if (!Utils::TryGetFileRevision(source.m_first, current) || current != *source.m_second)
			{
				return false;
			}
		}
		return true;
	}

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

bool TextureImporter::CaptureCpuDecodeRequest(const TextureAssetInfo& assetInfo,
	CpuDecodeRequest& outRequest)
{
	outRequest = {};
	auto request = DescribeCpuTexture(assetInfo);
	FileRevision revision;
	if (!Utils::TryGetFileRevision(request.m_filepath, revision))
	{
		return false;
	}
	request.m_sourceRevisions.Add(request.m_filepath, revision);
	if (request.m_glbTextureIndex >= 0 && Utils::GetFileExtension(request.m_filepath) == "gltf")
	{
		// TinyGLTF reads external images and buffers while extracting an image.
		// Capture their revisions too; the document timestamp alone is insufficient.
		std::ifstream input(request.m_filepath, std::ios::binary);
		const auto document = nlohmann::json::parse(input, nullptr, false);
		if (document.is_discarded() || !document.is_object())
		{
			return false;
		}
		const auto folder = std::filesystem::path(request.m_filepath).parent_path();
		for (const char* collection : { "buffers", "images" })
		{
			const auto entries = document.find(collection);
			if (entries == document.end() || !entries->is_array())
			{
				continue;
			}
			for (const auto& entry : *entries)
			{
				if (!entry.is_object())
				{
					continue;
				}
				const auto uri = entry.find("uri");
				if (uri == entry.end() || !uri->is_string())
				{
					continue;
				}
				const auto encoded = uri->get<std::string>();
				if (encoded.empty() || encoded.starts_with("data:"))
				{
					continue;
				}
				std::string decoded;
				if (!tinygltf::URIDecode(encoded, &decoded, nullptr))
				{
					return false;
				}
				const auto path = (folder / decoded).lexically_normal().string();
				if (!Utils::TryGetFileRevision(path, revision))
				{
					return false;
				}
				request.m_sourceRevisions[path] = revision;
			}
		}
	}
	if (!HasCurrentTextureSources(request))
	{
		return false;
	}
	outRequest = std::move(request);
	return true;
}

bool TextureImporter::DecodeTextureCpu(const CpuDecodeRequest& request, ByteCode& decodedData,
	int32_t& width, int32_t& height, uint32_t& mipLevels)
{
	decodedData.Clear();
	width = height = 0;
	mipLevels = 1u;
	if (!HasCurrentTextureSources(request))
	{
		return false;
	}
	const bool decoded = ImportTexture(request, decodedData, width, height, mipLevels);
	if (!decoded || !HasCurrentTextureSources(request))
	{
		decodedData.Clear();
		width = height = 0;
		mipLevels = 1u;
		return false;
	}
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
	for (auto& instance : m_textures)
	{
		instance.m_second.m_texture.DestroyObject(m_allocator);
	}
}

TexturePtr TextureImporter::GetLoadedTexture(FileId uid)
{
	TextureEntry entry;
	m_textures.TryGet(uid, entry);
	return entry.m_texture;
}

Tasks::TaskPtr<TexturePtr> TextureImporter::GetLoadPromise(FileId uid)
{
	TextureEntry entry;
	m_textures.TryGet(uid, entry);
	return entry.m_load;
}

Tasks::TaskPtr<TVector<TextureImporter::CpuTextureSnapshot>> TextureImporter::CaptureCpuTextures(
	const TVector<TexturePtr>& textures)
{
	if (textures.IsEmpty())
	{
		return Tasks::TaskPtr<TVector<CpuTextureSnapshot>>::Make(TVector<CpuTextureSnapshot>{});
	}

	m_textures.LockAll();
	TVector<TexturePtr> loaded;
	TVector<Tasks::ITaskPtr> previous;
	loaded.Reserve(textures.Num());
	for (const auto& texture : textures)
	{
		TextureEntry* entry = nullptr;
		if (texture && m_textures.Find(texture->GetFileId(), entry))
		{
			loaded.Add(entry->m_texture);
			previous.Add(entry->m_lastAccess);
		}
		else
		{
			loaded.Add({});
		}
	}
	auto capture = Tasks::CreateTask<TVector<CpuTextureSnapshot>>("Capture texture CPU state",
		[textures, loaded]()
		{
			TVector<CpuTextureSnapshot> snapshots;
			snapshots.Reserve(textures.Num());
			for (size_t i = 0; i < textures.Num(); ++i)
			{
				auto texture = textures[i];
				if (loaded[i] && (loaded[i]->HasCpuData() || !texture || !texture->HasCpuData()))
				{
					texture = loaded[i];
				}
				CpuTextureSnapshot snapshot;
				if (texture)
				{
					snapshot.m_pixels = texture->m_decodedData;
					snapshot.m_width = texture->m_width;
					snapshot.m_height = texture->m_height;
					snapshot.m_source = texture->m_cpuSource;
					if (texture->m_rhiTexture)
					{
						snapshot.m_clamping = texture->m_rhiTexture->GetClamping();
					}
				}
				snapshots.Add(std::move(snapshot));
			}
			return snapshots;
		}, EThreadType::RHI);
	for (const auto& task : previous)
	{
		capture->Join(task);
	}
	for (const auto& texture : textures)
	{
		TextureEntry* entry = nullptr;
		if (texture && m_textures.Find(texture->GetFileId(), entry))
		{
			entry->m_lastAccess = capture;
		}
	}
	m_textures.UnlockAll();
	capture->Run();
	return capture;
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

uint64_t TextureImporter::CalculateTextureSamplersRevision(
	const TVector<uint32_t>& requestedIndices) const
{
	size_t result = Fnv1aOffsetBasis;
	m_textureSamplersLock.Lock();
	if (!m_textureSamplersBindings)
	{
		m_textureSamplersLock.Unlock();
		return 0ull;
	}

	for (const uint32_t requestedIndex : requestedIndices)
	{
		if (requestedIndex >= MaxTexturesInScene)
		{
			continue;
		}
		const uint64_t contentRevision =
			requestedIndex < m_textureSamplerSlotRevisions.Num() ?
			m_textureSamplerSlotRevisions[requestedIndex] : 0ull;
		HashCombine(result, requestedIndex, contentRevision);
	}
	m_textureSamplersLock.Unlock();
	return static_cast<uint64_t>(result);
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
	auto* assetInfo = dynamic_cast<TextureAssetInfo*>(inAssetInfo);
	if (!bWasExpired || !assetInfo)
	{
		return;
	}
	auto texture = GetLoadedTexture(assetInfo->GetFileId());
	if (!texture)
	{
		return;
	}

	auto* registry = App::GetSubmodule<AssetRegistry>();
	const auto token = registry->BeginAssetProcessing(assetInfo);
	const auto uid = assetInfo->GetFileId();
	auto& entry = m_textures.At_Lock(uid);
	entry.m_bCpuBuffersRequested = assetInfo->ShouldKeepCpuBuffers();
	if (token)
	{
		entry.m_load = CreateTextureTask(texture, *assetInfo, false, true, entry.m_lastAccess);
	}
	else
	{
		// Rejected reloads still preserve the publication/read ordering for this texture.
		entry.m_load = Tasks::CreateTask<TexturePtr>("Reject texture reload", []() { return TexturePtr{}; }, EThreadType::RHI);
		entry.m_load->Join(entry.m_lastAccess);
	}
	entry.m_lastAccess = entry.m_load;
	auto task = entry.m_load;
	m_textures.Unlock(uid);
	auto acknowledge = Tasks::CreateTask<bool>("Acknowledge texture reload", [registry, token, task]()
		{
			const bool succeeded = task->GetResult().IsValid();
			registry->CompleteAssetProcessing(token, succeeded);
			return succeeded;
		});
	acknowledge->Join(task);
	registry->TrackScanProcessingTask(acknowledge);
	task->Run();
	acknowledge->Run();
}

void TextureImporter::OnImportAsset(AssetInfoPtr assetInfo)
{
}

bool TextureImporter::IsTextureLoaded(FileId uid) const
{
	return m_textures.ContainsKey(uid);
}

bool TextureImporter::ImportTexture(FileId uid, ByteCode& decodedData, int32_t& width, int32_t& height, uint32_t& mipLevels)
{
	if (auto* assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<TextureAssetInfoPtr>(uid))
	{
		return ImportTexture(DescribeCpuTexture(*assetInfo), decodedData, width, height, mipLevels);
	}
	return false;
}

bool TextureImporter::ImportTexture(const CpuDecodeRequest& request, ByteCode& decodedData,
	int32_t& width, int32_t& height, uint32_t& mipLevels)
{
	SAILOR_PROFILE_FUNCTION();
	const bool bDecodeAsFloat = request.m_bDecodeAsFloat;

	if (request.m_glbTextureIndex != -1)
	{
		const std::string extension =
			Utils::GetFileExtension(request.m_filepath.c_str());
		const bool bIsGlb = extension == "glb";
		const bool bIsGltf = extension == "gltf";

		if (!bIsGlb && !bIsGltf)
		{
			return false;
		}

		ByteCode rawBuffer;
		std::string extractionDiagnostic;
		const bool bExtracted = bIsGlb ?
			ExtractTextureFromGLB(
				request.m_filepath.c_str(),
				request.m_glbTextureIndex,
				rawBuffer) :
			ExtractTextureFromGltf(
				request.m_filepath,
				request.m_glbTextureIndex,
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
			const std::string filepath = request.m_filepath;

			if (bDecodeAsFloat)
			{
				if (float* pPixels = stbi_loadf_from_memory(&rawBuffer[0], (uint32_t)rawBuffer.Num(), &width, &height, &texChannels, STBI_rgb_alpha))
				{
					const uint32_t imageSize = (uint32_t)width * height * sizeof(float) * 4;
					decodedData.Resize(imageSize);
					memcpy(decodedData.GetData(), pPixels, imageSize);

					mipLevels = request.m_bGenerateMips ? static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1 : 1;
					stbi_image_free(pPixels);
					return true;
				}
			}
			else if (stbi_uc* pPixels = stbi_load_from_memory(&rawBuffer[0], (uint32_t)rawBuffer.Num(), &width, &height, &texChannels, STBI_rgb_alpha))
			{
				const uint32_t imageSize = (uint32_t)width * height * 4;
				decodedData.Resize(imageSize);
				memcpy(decodedData.GetData(), pPixels, imageSize);

				mipLevels = request.m_bGenerateMips ? static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1 : 1;
				stbi_image_free(pPixels);
				return true;
			}
		}
		else
		{
			SAILOR_LOG_ERROR(
				"Cannot extract texture %d from model source '%s' for asset %s: %s",
				request.m_glbTextureIndex,
				request.m_filepath.c_str(),
				request.m_fileId.ToString().c_str(),
				extractionDiagnostic.c_str());
		}

		return false;
	}
	else
	{
		int32_t texChannels = 0;
		const std::string filepath = request.m_filepath;

		if (bDecodeAsFloat)
		{
			if (float* pPixels = stbi_loadf(filepath.c_str(), &width, &height, &texChannels, STBI_rgb_alpha))
			{
				const uint32_t imageSize = (uint32_t)width * height * sizeof(float) * 4;
				decodedData.Resize(imageSize);
				memcpy(decodedData.GetData(), pPixels, imageSize);

				mipLevels = request.m_bGenerateMips ? static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1 : 1;
				stbi_image_free(pPixels);
				return true;
			}
		}
		else if (stbi_uc* pPixels = stbi_load(filepath.c_str(), &width, &height, &texChannels, STBI_rgb_alpha))
		{
			const uint32_t imageSize = (uint32_t)width * height * 4;
			decodedData.Resize(imageSize);
			memcpy(decodedData.GetData(), pPixels, imageSize);

			mipLevels = request.m_bGenerateMips ? static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1 : 1;
			stbi_image_free(pPixels);
			return true;
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
	if (!task)
	{
		outTexture = nullptr;
		return false;
	}

	task->Wait();
	return task->GetResult().IsValid();
}

Tasks::TaskPtr<TexturePtr> TextureImporter::CreateTextureTask(
	TexturePtr texture, const TextureAssetInfo& assetInfo, bool bCpuOnly, bool bHotReload,
	const Tasks::ITaskPtr& previous)
{
	CpuDecodeRequest source;
	if (!CaptureCpuDecodeRequest(assetInfo, source))
	{
		// A failed request still joins the preceding publication.
		source = DescribeCpuTexture(assetInfo);
	}
	const auto format = assetInfo.GetFormat();
	const auto filtration = assetInfo.GetFiltration();
	const auto clamping = assetInfo.GetClamping();
	const auto reduction = assetInfo.GetSamplerReduction();
	const auto usage = assetInfo.ShouldSupportStorageBinding() ?
		DefaultTextureUsage | RHI::ETextureUsageBit::Storage_Bit : DefaultTextureUsage;
	const bool bKeepCpu = assetInfo.ShouldKeepCpuBuffers();

	struct Data
	{
		ByteCode m_pixels;
		int32_t m_width = 0, m_height = 0;
		uint32_t m_mipLevels = 1;
		bool m_bDecoded = false;
	};
	auto decode = Tasks::CreateTask<TSharedPtr<Data>>("Decode texture",
		[source, decodeTexture = m_decodeTexture]()
		{
			auto data = TSharedPtr<Data>::Make();
			data->m_bDecoded = decodeTexture(source, data->m_pixels, data->m_width,
				data->m_height, data->m_mipLevels);
			return data;
		}, EThreadType::Worker);
	auto publish = decode->Then<TexturePtr>(
		[this, texture, source, format, filtration, clamping, reduction, usage,
			bCpuOnly, bKeepCpu, bHotReload](TSharedPtr<Data> data) mutable
		{
			if (!data->m_bDecoded || data->m_pixels.IsEmpty() || !HasCurrentTextureSources(source))
			{
				SAILOR_LOG_ERROR("Cannot load texture '%s': decoding failed or the source changed.",
					source.m_filepath.c_str());
				return TexturePtr{};
			}

			if (bCpuOnly)
			{
				if (texture->m_cpuSource != source)
				{
					SAILOR_LOG_ERROR("Cannot retain CPU texture '%s': reload its changed GPU source first.",
						source.m_filepath.c_str());
					return TexturePtr{};
				}
				if (!texture->HasCpuData())
				{
					texture->SetDecodedData(std::move(data->m_pixels));
				}
				return texture;
			}

			auto& driver = RHI::Renderer::GetDriver();
			auto rhi = driver->CreateTexture(data->m_pixels.GetData(), data->m_pixels.Num(),
				glm::vec3(data->m_width, data->m_height, 1), data->m_mipLevels,
				RHI::ETextureType::Texture2D, format, filtration, clamping, usage, reduction);
			if (!rhi)
			{
				return TexturePtr{};
			}
			driver->SetDebugName(rhi, source.m_filepath);

			size_t index = 0;
			m_textureSamplersIndices.TryGet(source.m_fileId, index);
			if (IsUserTextureSamplerIndexValid(index))
			{
				if (!UpdateTextureSamplerBinding(rhi, static_cast<uint32_t>(index)))
				{
					return TexturePtr{};
				}
			}
			else
			{
				if (!RegisterTextureSamplerBinding(rhi, index))
				{
					SAILOR_LOG_ERROR("Cannot register texture sampler '%s'.", source.m_filepath.c_str());
					return TexturePtr{};
				}
				m_textureSamplersIndices.At_Lock(source.m_fileId) = index;
				m_textureSamplersIndices.Unlock(source.m_fileId);
			}

			texture->m_rhiTexture = std::move(rhi);
			texture->m_width = data->m_width;
			texture->m_height = data->m_height;
			texture->m_mipLevels = data->m_mipLevels;
			texture->m_cpuSource = source;
			texture->SetDecodedData(bKeepCpu ? std::move(data->m_pixels) : ByteCode{});
			if (bHotReload)
			{
				texture->TraceHotReload(nullptr);
			}
			return texture;
		}, "Publish texture", EThreadType::RHI);
	// Decodes may overlap, but one texture's publications follow request order.
	if (previous)
	{
		publish->Join(previous);
	}
	return publish->ToTaskWithResult();
}

Tasks::TaskPtr<TexturePtr> TextureImporter::LoadTexture(FileId uid, TexturePtr& outTexture)
{
	SAILOR_PROFILE_FUNCTION();
	auto* assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<TextureAssetInfoPtr>(uid);
	if (!assetInfo)
	{
		outTexture = nullptr;
		return {};
	}

	auto& entry = m_textures.At_Lock(uid);
	auto& promise = entry.m_load;
	auto& texture = entry.m_texture;
	const bool bKeepCpu = assetInfo->ShouldKeepCpuBuffers();
	const bool bPending = promise && !promise->IsFinished();
	bool bCpuOnly = false;
	if (bPending)
	{
		if (!bKeepCpu || entry.m_bCpuBuffersRequested)
		{
			outTexture = texture;
			auto task = promise;
			m_textures.Unlock(uid);
			return task;
		}
		bCpuOnly = true;
	}
	else if (texture && texture->GetRHI())
	{
		if (!bKeepCpu || texture->HasCpuData())
		{
			outTexture = texture;
			auto task = Tasks::TaskPtr<TexturePtr>::Make(texture);
			m_textures.Unlock(uid);
			return task;
		}
		bCpuOnly = true;
	}
	else if (!texture)
	{
		texture = TexturePtr::Make(m_allocator, uid);
	}

	entry.m_bCpuBuffersRequested = bKeepCpu;
	promise = CreateTextureTask(texture, *assetInfo, bCpuOnly, false, entry.m_lastAccess);
	entry.m_lastAccess = promise;
	outTexture = texture;
	auto task = promise;
	m_textures.Unlock(uid);
	task->Run();
	return task;
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
	m_textures.LockAll();
	for (auto& entry : m_textures)
	{
		if (entry.m_second.m_load && entry.m_second.m_load->IsFinished())
		{
			entry.m_second.m_load.Clear();
		}
		if (entry.m_second.m_lastAccess && entry.m_second.m_lastAccess->IsFinished())
		{
			entry.m_second.m_lastAccess.Clear();
		}
	}
	m_textures.UnlockAll();
}
