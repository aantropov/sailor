#include "AssetRegistry/Model/ModelImporter.h"

#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/GltfImporterUtils.h"
#include "AssetRegistry/Model/ModelGeometry.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Core/Utils.h"
#include "Raytracing/PathTracer.h"
#include "RHI/Renderer.h"
#include "Workspace/WorkspaceCacheContract.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>

#include <tiny_gltf.h>
#include <stb_image_write.h>

using namespace Sailor;

namespace
{
	constexpr size_t MaxDecodedImageBytes = 256ull * 1024ull * 1024ull;
	constexpr size_t MaxEncodedImageBytes = 4ull * 1024ull * 1024ull;
	constexpr int32_t MaxTextureDimension = 256;
	constexpr int32_t ImageDimension = 256;

	struct FingerprintRequest final
	{
		uint64_t m_generation = 0;
		FileRevision m_sourceRevision{};

		bool operator==(const FingerprintRequest& rhs) const noexcept
		{
			return m_generation == rhs.m_generation && m_sourceRevision == rhs.m_sourceRevision;
		}
	};

	std::mutex g_fingerprintRequestsMutex;
	uint64_t g_nextFingerprintGeneration = 0u;
	TMap<FileId, FingerprintRequest> g_fingerprintRequests;

	bool IsFingerprintRequestCurrentLocked(const FileId& fileId, const FingerprintRequest& request)
	{
		FingerprintRequest* current = nullptr;
		return g_fingerprintRequests.Find(fileId, current) && current != nullptr && *current == request;
	}

	FingerprintRequest BeginFingerprintRequest(const FileId& fileId,
		const FileRevision& sourceRevision,
		const std::filesystem::path& outputPath,
		std::error_code& outRemoveError)
	{
		const std::lock_guard<std::mutex> lock(g_fingerprintRequestsMutex);
		const FingerprintRequest request{++g_nextFingerprintGeneration, sourceRevision};
		g_fingerprintRequests[fileId] = request;
		std::filesystem::remove(outputPath, outRemoveError);
		return request;
	}

	bool IsFingerprintRequestCurrent(const FileId& fileId, const FingerprintRequest& request)
	{
		const std::lock_guard<std::mutex> lock(g_fingerprintRequestsMutex);
		return IsFingerprintRequestCurrentLocked(fileId, request);
	}

	void CompleteFingerprintRequest(const FileId& fileId, const FingerprintRequest& request)
	{
		const std::lock_guard<std::mutex> lock(g_fingerprintRequestsMutex);
		if (IsFingerprintRequestCurrentLocked(fileId, request))
		{
			g_fingerprintRequests.Remove(fileId);
		}
	}

	bool PublishFingerprint(const FileId& fileId,
		const FingerprintRequest& request,
		const std::filesystem::path& outputPath,
		const TVector<uint8_t>& bytes,
		std::string& outDiagnostic)
	{
		const std::lock_guard<std::mutex> lock(g_fingerprintRequestsMutex);
		return IsFingerprintRequestCurrentLocked(fileId, request) &&
			   Workspace::AtomicReplaceWorkspaceCacheBinary(
				   outputPath, bytes.GetData(), static_cast<uint64_t>(bytes.Num()), outDiagnostic);
	}

	std::filesystem::path GetFingerprintPath(const FileId& fileId)
	{
		const std::filesystem::path filename = fileId.ToString() + ".png";
		if (!fileId || filename != filename.filename())
		{
			return {};
		}

		return std::filesystem::path(AssetRegistry::GetCacheFolder()) / "Fingerprints" / filename;
	}

	struct EncodedFingerprint
	{
		TVector<uint8_t> m_bytes;
		bool m_bValid = true;
	};

	void AppendEncodedFingerprintBytes(void* context, void* data, int32_t size) noexcept
	{
		EncodedFingerprint* encoded = static_cast<EncodedFingerprint*>(context);
		if (encoded == nullptr || !encoded->m_bValid || data == nullptr || size <= 0 ||
			encoded->m_bytes.Num() > MaxEncodedImageBytes ||
			static_cast<size_t>(size) > MaxEncodedImageBytes - encoded->m_bytes.Num())
		{
			if (encoded != nullptr)
			{
				encoded->m_bValid = false;
			}
			return;
		}

		const size_t previousSize = encoded->m_bytes.Num();
		encoded->m_bytes.Resize(previousSize + static_cast<size_t>(size));
		std::memcpy(encoded->m_bytes.GetData() + previousSize, data, static_cast<size_t>(size));
	}

	bool TryMultiplySize(size_t lhs, size_t rhs, size_t& outResult)
	{
		if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs)
		{
			return false;
		}

		outResult = lhs * rhs;
		return true;
	}

	bool TryCalculateFingerprintTextureSize(int32_t sourceWidth,
		int32_t sourceHeight,
		int32_t& outWidth,
		int32_t& outHeight,
		size_t& outBytes)
	{
		if (sourceWidth <= 0 || sourceHeight <= 0)
		{
			return false;
		}

		outWidth = sourceWidth;
		outHeight = sourceHeight;
		if (sourceWidth > MaxTextureDimension || sourceHeight > MaxTextureDimension)
		{
			if (sourceWidth >= sourceHeight)
			{
				outWidth = MaxTextureDimension;
				outHeight = static_cast<int32_t>((std::max)(uint64_t{1},
					(static_cast<uint64_t>(sourceHeight) * MaxTextureDimension +
						static_cast<uint64_t>(sourceWidth) / 2) /
						static_cast<uint64_t>(sourceWidth)));
			}
			else
			{
				outHeight = MaxTextureDimension;
				outWidth = static_cast<int32_t>((std::max)(uint64_t{1},
					(static_cast<uint64_t>(sourceWidth) * MaxTextureDimension +
						static_cast<uint64_t>(sourceHeight) / 2) /
						static_cast<uint64_t>(sourceHeight)));
			}
		}

		size_t pixelCount = 0;
		return outWidth > 0 && outHeight > 0 && outWidth <= MaxTextureDimension && outHeight <= MaxTextureDimension &&
			   TryMultiplySize(static_cast<size_t>(outWidth), static_cast<size_t>(outHeight), pixelCount) &&
			   TryMultiplySize(pixelCount, sizeof(u8vec4), outBytes);
	}
}

void ModelImporter::GenerateFingerprintAsync(ModelAssetInfoPtr modelAssetInfo)
{
	if (!modelAssetInfo || !modelAssetInfo->GetFileId())
	{
		return;
	}

	const FileId fileId = modelAssetInfo->GetFileId();
	const std::filesystem::path outputPath = GetFingerprintPath(fileId);
	if (outputPath.empty())
	{
		SAILOR_LOG_ERROR("Cannot generate fingerprint for invalid FileId: %s", fileId.ToString().c_str());
		return;
	}

	const std::string assetFilepath = modelAssetInfo->GetAssetFilepath();
	const float unitScale = modelAssetInfo->GetUnitScale();
	const bool bShouldBatchByMaterial = modelAssetInfo->ShouldBatchByMaterial();
	const bool bFlipTexcoordY = modelAssetInfo->ShouldFlipTexcoordY();
	FileRevision sourceRevision;
	if (!Utils::TryGetFileRevision(assetFilepath, sourceRevision))
	{
		SAILOR_LOG_ERROR("Cannot capture model source revision for fingerprint: %s", assetFilepath.c_str());
		return;
	}

	if (App::GetSubmodule<Tasks::Scheduler>() == nullptr)
	{
		SAILOR_LOG_ERROR("Cannot schedule model fingerprint without a task scheduler: %s", assetFilepath.c_str());
		return;
	}

	std::error_code removeError;
	const FingerprintRequest request = BeginFingerprintRequest(fileId, sourceRevision, outputPath, removeError);
	if (removeError)
	{
		SAILOR_LOG_ERROR("Cannot invalidate previous model fingerprint '%s': %s",
			outputPath.string().c_str(),
			removeError.message().c_str());
	}

	Tasks::CreateTask(
		"Generate model fingerprint",
		[fileId, assetFilepath, unitScale, bShouldBatchByMaterial, bFlipTexcoordY, outputPath, request]()
		{
			if (IsFingerprintRequestCurrent(fileId, request))
			{
				GenerateFingerprint(fileId,
					assetFilepath,
					unitScale,
					bShouldBatchByMaterial,
					bFlipTexcoordY,
					outputPath.string(),
					request.m_generation,
					request.m_sourceRevision);
			}
			CompleteFingerprintRequest(fileId, request);
		},
		EThreadType::Background)
		->Run();
}

bool ModelImporter::GenerateFingerprint(const FileId& fileId,
	const std::string& assetFilepath,
	float unitScale,
	bool bShouldBatchByMaterial,
	bool bFlipTexcoordY,
	const std::string& outputPath,
	uint64_t requestGeneration,
	const FileRevision& sourceRevision)
{
	TVector<MeshContext> parsedMeshes;
	TVector<glm::mat4> inverseBind;
	Math::AABB boundsAabb;
	Math::Sphere boundsSphere;
	tinygltf::Model gltfModel;
	if (!ImportModel(assetFilepath,
			unitScale,
			bShouldBatchByMaterial,
			bFlipTexcoordY,
			parsedMeshes,
			boundsAabb,
			boundsSphere,
			inverseBind,
			&gltfModel) ||
		parsedMeshes.Num() == 0 || !boundsAabb.IsValid())
	{
		SAILOR_LOG_ERROR("Cannot prepare model fingerprint: %s", assetFilepath.c_str());
		return false;
	}
	TVector<GltfImporterUtils::SceneNode> sceneNodes;
	if (!GltfImporterUtils::CollectSceneNodes(gltfModel, unitScale, sceneNodes))
	{
		SAILOR_LOG_ERROR("Cannot resolve model fingerprint hierarchy: %s", assetFilepath.c_str());
		return false;
	}

	ObjectAllocatorPtr allocator = ObjectAllocatorPtr::Make(EAllocationPolicy::SharedMemory_MultiThreaded);
	const size_t previewMaterialCount = (std::min)(gltfModel.materials.size(), size_t{256});
	TVector<MaterialPtr> previewMaterials(previewMaterialCount);
	TMap<int32_t, int32_t> previewTextureSources;
	TMap<int32_t, TexturePtr> previewImages;

	auto resolvePreviewImageSource = [&](int32_t textureIndex) -> int32_t
	{
		int32_t* cachedSource = nullptr;
		if (previewTextureSources.Find(textureIndex, cachedSource) && cachedSource != nullptr)
		{
			return *cachedSource;
		}

		int32_t imageIndex = -1;
		if (textureIndex >= 0 && static_cast<size_t>(textureIndex) < gltfModel.textures.size())
		{
			const tinygltf::Texture& sourceTexture = gltfModel.textures[textureIndex];
			if (sourceTexture.source >= 0 && static_cast<size_t>(sourceTexture.source) < gltfModel.images.size())
			{
				imageIndex = sourceTexture.source;
			}
		}

		previewTextureSources[textureIndex] = imageIndex;
		return imageIndex;
	};

	auto loadPreviewTexture = [&](int32_t textureIndex) -> TexturePtr
	{
		const int32_t imageIndex = resolvePreviewImageSource(textureIndex);
		if (imageIndex < 0)
		{
			return TexturePtr();
		}

		TexturePtr* cachedTexture = nullptr;
		if (previewImages.Find(imageIndex, cachedTexture) && cachedTexture != nullptr)
		{
			return *cachedTexture;
		}

		const tinygltf::Image& image = gltfModel.images[imageIndex];
		auto cacheFailure = [&]() -> TexturePtr
		{
			previewImages[imageIndex] = TexturePtr();
			return TexturePtr();
		};

		if (!image.as_is || image.image.empty() ||
			image.image.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
		{
			return cacheFailure();
		}

		int32_t sourceWidth = 0;
		int32_t sourceHeight = 0;
		int32_t sourceChannels = 0;
		if (!stbi_info_from_memory(image.image.data(),
				static_cast<int>(image.image.size()),
				&sourceWidth,
				&sourceHeight,
				&sourceChannels) ||
			sourceWidth <= 0 || sourceHeight <= 0 || sourceChannels <= 0)
		{
			return cacheFailure();
		}

		size_t sourcePixelCount = 0;
		size_t decodedBytes = 0;
		if (!TryMultiplySize(static_cast<size_t>(sourceWidth), static_cast<size_t>(sourceHeight), sourcePixelCount) ||
			!TryMultiplySize(sourcePixelCount, static_cast<size_t>(STBI_rgb_alpha), decodedBytes) ||
			decodedBytes > MaxDecodedImageBytes)
		{
			return cacheFailure();
		}

		int32_t decodedWidth = 0;
		int32_t decodedHeight = 0;
		int32_t decodedChannels = 0;
		std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decodedPixels(stbi_load_from_memory(image.image.data(),
																			   static_cast<int>(image.image.size()),
																			   &decodedWidth,
																			   &decodedHeight,
																			   &decodedChannels,
																			   STBI_rgb_alpha),
			stbi_image_free);
		if (!decodedPixels || decodedWidth != sourceWidth || decodedHeight != sourceHeight)
		{
			return cacheFailure();
		}

		int32_t previewWidth = 0;
		int32_t previewHeight = 0;
		size_t previewBytes = 0;
		if (!TryCalculateFingerprintTextureSize(decodedWidth, decodedHeight, previewWidth, previewHeight, previewBytes))
		{
			return cacheFailure();
		}

		TVector<uint8_t> previewPixels;
		previewPixels.Resize(previewBytes);
		u8vec4* destination = reinterpret_cast<u8vec4*>(previewPixels.GetData());
		for (int32_t y = 0; y < previewHeight; ++y)
		{
			const size_t sourceY = (std::min)(static_cast<size_t>(decodedHeight - 1),
				static_cast<size_t>((static_cast<uint64_t>(y) * decodedHeight) / previewHeight));
			for (int32_t x = 0; x < previewWidth; ++x)
			{
				const size_t sourceX = (std::min)(static_cast<size_t>(decodedWidth - 1),
					static_cast<size_t>((static_cast<uint64_t>(x) * decodedWidth) / previewWidth));
				const size_t sourceOffset =
					(sourceY * static_cast<size_t>(decodedWidth) + sourceX) * static_cast<size_t>(STBI_rgb_alpha);
				const size_t destinationIndex =
					static_cast<size_t>(y) * static_cast<size_t>(previewWidth) + static_cast<size_t>(x);
				destination[destinationIndex] = u8vec4(decodedPixels.get()[sourceOffset + 0],
					decodedPixels.get()[sourceOffset + 1],
					decodedPixels.get()[sourceOffset + 2],
					decodedPixels.get()[sourceOffset + 3]);
			}
		}

		decodedPixels.reset();
		TexturePtr texture = TexturePtr::Make(allocator, FileId::CreateNewFileId());
		texture->m_decodedData = std::move(previewPixels);
		texture->m_width = previewWidth;
		texture->m_height = previewHeight;
		texture->m_mipLevels = 1;

		previewImages[imageIndex] = texture;
		return texture;
	};

	for (size_t i = 0; i < previewMaterialCount; i++)
	{
		const tinygltf::Material& sourceMaterial = gltfModel.materials[i];
		const auto transmissionSettings =
			GltfImporterUtils::ResolveMaterialTransmission(sourceMaterial, gltfModel.textures.size(), unitScale);
		const auto alphaModeSettings =
			GltfImporterUtils::ResolveMaterialAlphaMode(sourceMaterial.alphaMode, transmissionSettings.IsEnabled());

		MaterialPtr material = MaterialPtr::Make(allocator, FileId::CreateNewFileId());
		material->SetRenderState(RHI::RenderState(true,
			alphaModeSettings.m_bEnableZWrite,
			0.0f,
			alphaModeSettings.m_bAlphaCutout,
			sourceMaterial.doubleSided ? RHI::ECullMode::None : RHI::ECullMode::Back,
			alphaModeSettings.m_blendMode,
			RHI::EFillMode::Fill,
			StringHash::Runtime(alphaModeSettings.m_renderQueue).GetHash()));

		const auto& pbr = sourceMaterial.pbrMetallicRoughness;
		material->SetUniform("material.baseColorFactor",
			vec4(pbr.baseColorFactor[0], pbr.baseColorFactor[1], pbr.baseColorFactor[2], pbr.baseColorFactor[3]));
		material->SetUniform(
			"material.emissiveFactor", vec4(GltfImporterUtils::ResolveMaterialEmissiveFactor(sourceMaterial), 0.0f));
		material->SetUniform("material.roughnessFactor", static_cast<float>(pbr.roughnessFactor));
		material->SetUniform("material.metallicFactor", static_cast<float>(pbr.metallicFactor));
		material->SetUniform("material.alphaCutoff", static_cast<float>(sourceMaterial.alphaCutoff));
		if (transmissionSettings.IsEnabled())
		{
			material->SetUniform("material.transmissionFactor", transmissionSettings.m_factor);
			material->SetUniform("material.thicknessFactor", transmissionSettings.m_thicknessFactor);
			material->SetUniform("material.attenuationDistance", transmissionSettings.m_attenuationDistance);
			material->SetUniform("material.attenuationColor", glm::vec4(transmissionSettings.m_attenuationColor, 1.0f));
		}
		if (transmissionSettings.IsEnabled() || transmissionSettings.m_bHasIndexOfRefraction)
		{
			material->SetUniform("material.indexOfRefraction", transmissionSettings.m_indexOfRefraction);
		}

		auto bindTexture = [&](const char* samplerName, int32_t textureIndex)
		{
			if (TexturePtr texture = loadPreviewTexture(textureIndex))
			{
				material->SetSampler(samplerName, texture);
			}
		};
		bindTexture("baseColorSampler", pbr.baseColorTexture.index);
		bindTexture("normalSampler", sourceMaterial.normalTexture.index);
		bindTexture("ormSampler", pbr.metallicRoughnessTexture.index);
		bindTexture("emissiveSampler", sourceMaterial.emissiveTexture.index);
		bindTexture("occlusionSampler", sourceMaterial.occlusionTexture.index);
		if (transmissionSettings.IsEnabled())
		{
			bindTexture("transmissionSampler", transmissionSettings.m_textureIndex);
			bindTexture("thicknessSampler", transmissionSettings.m_thicknessTextureIndex);
		}
		previewMaterials[i] = std::move(material);
	}
	ModelPtr model = ModelPtr::Make(allocator, fileId);
	model->m_boundsAabb = boundsAabb;
	model->m_boundsSphere = boundsSphere;
	model->m_inverseBind = std::move(inverseBind);
	model->m_cpuMeshes.Reserve(parsedMeshes.Num());
	model->m_sourceMeshes.Resize(gltfModel.meshes.size());

	for (MeshContext& mesh : parsedMeshes)
	{
		if (!mesh.HasGeometry())
		{
			continue;
		}

		Model::MeshCpuData cpuMesh{};
		cpuMesh.m_vertices = std::move(mesh.outVertices);
		for (auto& vertex : cpuMesh.m_vertices)
		{
			ModelGeometry::SanitizeVertexFrame(vertex);
		}
		cpuMesh.m_indices = std::move(mesh.outIndices);
		cpuMesh.m_bounds = mesh.bounds;
		cpuMesh.m_materialIndex = mesh.materialIndex;
		const uint32_t renderMeshIndex = static_cast<uint32_t>(model->m_cpuMeshes.Num());
		model->m_cpuMeshes.Add(std::move(cpuMesh));
		if (mesh.sourceMeshIndex >= 0 && static_cast<size_t>(mesh.sourceMeshIndex) < model->m_sourceMeshes.Num())
		{
			auto& sourceMesh = model->m_sourceMeshes[static_cast<size_t>(mesh.sourceMeshIndex)];
			sourceMesh.m_renderMeshIndices.Add(renderMeshIndex);
			sourceMesh.m_bounds.Extend(mesh.bounds);
		}
	}

	PopulateModelSceneHierarchy(*model, sceneNodes);
	gltfModel = tinygltf::Model();

	if (!model->BuildBLAS())
	{
		SAILOR_LOG_ERROR("Cannot build model fingerprint BLAS: %s", assetFilepath.c_str());
		return false;
	}

	Raytracing::PathTracer::TLASInstance instance{};
	instance.m_model = model;
	instance.m_worldBounds = boundsAabb;

	Raytracing::PathTracer pathTracer;
	if (!pathTracer.InitializeScene(TVector<Raytracing::PathTracer::TLASInstance>{instance}, previewMaterials, {}))
	{
		SAILOR_LOG_ERROR("Cannot initialize model fingerprint scene: %s", assetFilepath.c_str());
		return false;
	}

	const float radius = (std::max)(boundsSphere.m_radius, 0.1f);
	Raytracing::PathTracer::Params params{};
	params.m_height = ImageDimension;
	params.m_numSamples = 1;
	params.m_numAmbientSamples = 1;
	params.m_maxBounces = 4;
	params.m_msaa = 1;
	params.m_ambient = vec3(0.08f);
	params.m_rayBiasScale = 3e-4f;
	params.m_bUseRuntimeCamera = true;
	params.m_runtimeCameraPos = boundsSphere.m_center + vec3(0.0f, radius * 0.6f, radius * 2.5f);
	params.m_runtimeCameraForward = glm::normalize(boundsSphere.m_center - params.m_runtimeCameraPos);
	params.m_runtimeAspectRatio = 1.0f;
	params.m_bRunTasksInline = true;

	if (!pathTracer.RenderPreparedScene(params))
	{
		SAILOR_LOG_ERROR("Cannot render model fingerprint: %s", assetFilepath.c_str());
		return false;
	}

	const glm::uvec2 extent = pathTracer.GetLastRenderedExtent();
	const TVector<u8vec4>& renderedImage = pathTracer.GetLastRenderedImage();
	size_t expectedPixelCount = 0;
	if (extent.x != ImageDimension || extent.y != ImageDimension ||
		!TryMultiplySize(static_cast<size_t>(extent.x), static_cast<size_t>(extent.y), expectedPixelCount) ||
		renderedImage.Num() != expectedPixelCount)
	{
		SAILOR_LOG_ERROR("Model fingerprint renderer returned an invalid image: %s", assetFilepath.c_str());
		return false;
	}

	EncodedFingerprint encoded;
	const int32_t channels = 4;
	if (!stbi_write_png_to_func(AppendEncodedFingerprintBytes,
			&encoded,
			static_cast<int32_t>(extent.x),
			static_cast<int32_t>(extent.y),
			channels,
			renderedImage.GetData(),
			static_cast<int32_t>(extent.x) * channels) ||
		!encoded.m_bValid || encoded.m_bytes.IsEmpty() || encoded.m_bytes.Num() > MaxEncodedImageBytes)
	{
		SAILOR_LOG_ERROR("Cannot encode model fingerprint: %s", assetFilepath.c_str());
		return false;
	}

	int32_t encodedWidth = 0;
	int32_t encodedHeight = 0;
	int32_t encodedChannels = 0;
	if (!stbi_info_from_memory(encoded.m_bytes.GetData(),
			static_cast<int>(encoded.m_bytes.Num()),
			&encodedWidth,
			&encodedHeight,
			&encodedChannels) ||
		encodedWidth != ImageDimension || encodedHeight != ImageDimension || encodedChannels <= 0)
	{
		SAILOR_LOG_ERROR("Encoded model fingerprint is invalid: %s", assetFilepath.c_str());
		return false;
	}

	FileRevision currentSourceRevision;
	if (!Utils::TryGetFileRevision(assetFilepath, currentSourceRevision) || currentSourceRevision != sourceRevision)
	{
		SAILOR_LOG("Discarded stale model fingerprint: %s", assetFilepath.c_str());
		return false;
	}

	const FingerprintRequest request{requestGeneration, sourceRevision};
	std::string diagnostic;
	if (!PublishFingerprint(fileId, request, std::filesystem::path(outputPath), encoded.m_bytes, diagnostic))
	{
		if (diagnostic.empty())
		{
			SAILOR_LOG("Discarded superseded model fingerprint: %s", assetFilepath.c_str());
		}
		else
		{
			SAILOR_LOG_ERROR(
				"Cannot atomically publish model fingerprint '%s': %s", outputPath.c_str(), diagnostic.c_str());
		}
		return false;
	}

	SAILOR_LOG("Generated model fingerprint: %s", outputPath.c_str());
	return true;
}
