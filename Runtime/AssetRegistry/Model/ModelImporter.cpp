#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

#include "ModelImporter.h"
#include "AssetRegistry/FileId.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/GeneratedModelAssetMetadata.h"
#include "AssetRegistry/Model/ModelMiniature.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "ModelAssetInfo.h"
#include "Core/Utils.h"
#include "Raytracing/PathTracer.h"
#include "RHI/VertexDescription.h"
#include "RHI/Types.h"
#include "RHI/Renderer.h"
#include "Memory/ObjectAllocator.hpp"
#include "Tasks/Scheduler.h"
#include "Workspace/WorkspaceCacheContract.h"

#ifndef TINYGLTF_IMPLEMENTATION
#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>
#endif

using namespace Sailor;

namespace
{
	constexpr float BasisLengthEpsilon = 1e-12f;

	bool IsFiniteVector(const vec3& value)
	{
		return std::isfinite(value.x) &&
			std::isfinite(value.y) &&
			std::isfinite(value.z);
	}

	vec3 NormalizeOrFallback(const vec3& value, const vec3& fallback)
	{
		if (IsFiniteVector(value) && glm::dot(value, value) > BasisLengthEpsilon)
		{
			return glm::normalize(value);
		}

		if (IsFiniteVector(fallback) &&
			glm::dot(fallback, fallback) > BasisLengthEpsilon)
		{
			return glm::normalize(fallback);
		}

		return vec3(0.0f, 1.0f, 0.0f);
	}

	void BuildSafeBasis(
		const vec3& normalCandidate,
		const vec3& tangentCandidate,
		const vec3& bitangentCandidate,
		const vec3& geometricNormal,
		vec3& outNormal,
		vec3& outTangent,
		vec3& outBitangent)
	{
		outNormal = NormalizeOrFallback(normalCandidate, geometricNormal);
		const vec3 tangentFallback = glm::cross(
			glm::abs(outNormal.y) < 0.999f
				? vec3(0.0f, 1.0f, 0.0f)
				: vec3(1.0f, 0.0f, 0.0f),
			outNormal);
		const vec3 orthogonalTangent =
			tangentCandidate - outNormal * glm::dot(tangentCandidate, outNormal);
		outTangent = NormalizeOrFallback(orthogonalTangent, tangentFallback);

		const vec3 orthogonalBitangent =
			bitangentCandidate -
			outNormal * glm::dot(bitangentCandidate, outNormal) -
			outTangent * glm::dot(bitangentCandidate, outTangent);
		outBitangent = NormalizeOrFallback(
			orthogonalBitangent,
			glm::cross(outNormal, outTangent));
	}
}

YAML::Node Model::Serialize() const
{
	YAML::Node res;
	SERIALIZE_PROPERTY(res, m_fileId);
	return res;
}

void Model::Deserialize(const YAML::Node& inData)
{
	DESERIALIZE_PROPERTY(inData, m_fileId);
}

void Model::Flush()
{
	if (m_meshes.Num() == 0)
	{
		m_bIsReady = false;
		return;
	}

	for (const auto& mesh : m_meshes)
	{
		if (!mesh) // || !mesh->IsReady())
		{
			m_bIsReady = false;
			return;
		}
	}

	m_bIsReady = true;
}

bool Model::BuildBLAS()
{
	m_blas.Clear();
	m_blasTriangles.Clear();

	if (m_cpuMeshes.Num() == 0)
	{
		return false;
	}

	size_t expectedNumTriangles = 0;
	for (const auto& mesh : m_cpuMeshes)
	{
		expectedNumTriangles += mesh.m_indices.Num() / 3;
	}

	if (expectedNumTriangles == 0)
	{
		return false;
	}

	m_blasTriangles.Reserve(expectedNumTriangles);

	for (const auto& mesh : m_cpuMeshes)
	{
		for (size_t i = 0; i + 2 < mesh.m_indices.Num(); i += 3)
		{
			const uint32_t i0 = mesh.m_indices[i + 0];
			const uint32_t i1 = mesh.m_indices[i + 1];
			const uint32_t i2 = mesh.m_indices[i + 2];

			const auto& v0 = mesh.m_vertices[i0];
			const auto& v1 = mesh.m_vertices[i1];
			const auto& v2 = mesh.m_vertices[i2];

			Math::Triangle tri{};
			tri.m_vertices[0] = v0.m_position;
			tri.m_vertices[1] = v1.m_position;
			tri.m_vertices[2] = v2.m_position;

			const vec3 geometricNormal = glm::cross(
				tri.m_vertices[1] - tri.m_vertices[0],
				tri.m_vertices[2] - tri.m_vertices[0]);
			BuildSafeBasis(
				v0.m_normal,
				v0.m_tangent,
				v0.m_bitangent,
				geometricNormal,
				tri.m_normals[0],
				tri.m_tangent[0],
				tri.m_bitangent[0]);
			BuildSafeBasis(
				v1.m_normal,
				v1.m_tangent,
				v1.m_bitangent,
				geometricNormal,
				tri.m_normals[1],
				tri.m_tangent[1],
				tri.m_bitangent[1]);
			BuildSafeBasis(
				v2.m_normal,
				v2.m_tangent,
				v2.m_bitangent,
				geometricNormal,
				tri.m_normals[2],
				tri.m_tangent[2],
				tri.m_bitangent[2]);

			tri.m_uvs[0] = v0.m_texcoord;
			tri.m_uvs[1] = v1.m_texcoord;
			tri.m_uvs[2] = v2.m_texcoord;
			tri.m_uvs2[0] = tri.m_uvs[0];
			tri.m_uvs2[1] = tri.m_uvs[1];
			tri.m_uvs2[2] = tri.m_uvs[2];

			tri.m_materialIndex = static_cast<uint8_t>((std::max)(0, (std::min)(mesh.m_materialIndex, 255)));
			tri.m_centroid = (tri.m_vertices[0] + tri.m_vertices[1] + tri.m_vertices[2]) / 3.0f;

			m_blasTriangles.Add(tri);
		}
	}

	if (m_blasTriangles.Num() == 0)
	{
		return false;
	}

	m_blas = TSharedPtr<Raytracing::BVH>::Make((uint32_t)m_blasTriangles.Num());
	m_blas->BuildBVH(m_blasTriangles);
	return true;
}

void Model::ProceedCpuMeshes(bool bShouldGenerateBLAS, bool bShouldKeepCpuBuffers)
{
	if (bShouldGenerateBLAS)
	{
		BuildBLAS();
	}

	if (!bShouldKeepCpuBuffers)
	{
		m_cpuMeshes.Clear();
	}
}

bool Model::IsReady() const
{
	return m_bIsReady;
}

ModelImporter::ModelImporter(ModelAssetInfoHandler* infoHandler)
{
	SAILOR_PROFILE_FUNCTION();
	m_allocator = ObjectAllocatorPtr::Make(EAllocationPolicy::SharedMemory_MultiThreaded);
	infoHandler->Subscribe(this);
}

ModelImporter::~ModelImporter()
{
	for (auto& model : m_loadedModels)
	{
		model.m_second.DestroyObject(m_allocator);
	}
}

void ModelImporter::OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired)
{
	SAILOR_PROFILE_FUNCTION();
	SAILOR_PROFILE_TEXT(assetInfo->GetAssetFilepath().c_str());

	if (ModelAssetInfoPtr modelAssetInfo = dynamic_cast<ModelAssetInfoPtr>(assetInfo))
	{
		FileRevision sourceRevision;
		if (!Utils::TryGetFileRevision(
				modelAssetInfo->GetAssetFilepath(),
				sourceRevision))
		{
			if (AssetRegistry* assetRegistry =
					App::GetSubmodule<AssetRegistry>())
			{
				const AssetRegistry::AssetProcessingToken processingToken =
					assetRegistry->BeginAssetProcessing(modelAssetInfo);
				assetRegistry->CompleteAssetProcessing(
					processingToken,
					false);
			}
			SAILOR_LOG_ERROR(
				"Cannot capture model revision before generating dependent assets: %s",
				modelAssetInfo->GetAssetFilepath().c_str());
			return;
		}

		const bool bShouldPrepareGeneratedAssets =
			bWasExpired || modelAssetInfo->IsImportPending();
		bool bMetadataChanged = false;
		bool bGeneratedAssetsReady = true;

		if (modelAssetInfo->IsWritable() &&
			bShouldPrepareGeneratedAssets &&
			modelAssetInfo->ShouldGenerateMaterials() &&
			modelAssetInfo->GetDefaultMaterials().Num() == 0)
		{
			bGeneratedAssetsReady = GenerateMaterialAssets(modelAssetInfo);
			bMetadataChanged = bGeneratedAssetsReady;
		}
		if (!bGeneratedAssetsReady)
		{
			if (AssetRegistry* assetRegistry =
					App::GetSubmodule<AssetRegistry>())
			{
				const AssetRegistry::AssetProcessingToken processingToken =
					assetRegistry->BeginAssetProcessing(modelAssetInfo);
				assetRegistry->CompleteAssetProcessing(
					processingToken,
					false);
			}
			SAILOR_LOG_ERROR(
				"Cannot schedule a model miniature until generated material assets are ready: %s",
				modelAssetInfo->GetAssetFilepath().c_str());
			return;
		}

		if (modelAssetInfo->IsWritable() &&
			bShouldPrepareGeneratedAssets &&
			modelAssetInfo->GetAnimations().Num() == 0)
		{
			GenerateAnimationAssets(modelAssetInfo);
			bMetadataChanged = true;
		}

		if (bMetadataChanged)
		{
			assetInfo->SaveMetaFile();
		}

		const std::filesystem::path miniaturePath = ModelMiniature::GetCachePath(
			AssetRegistry::GetCacheFolder(),
			modelAssetInfo->GetFileId());
		if (miniaturePath.empty())
		{
			SAILOR_LOG_ERROR(
				"Cannot resolve a safe miniature cache path for model FileId: %s",
				modelAssetInfo->GetFileId().ToString().c_str());
			return;
		}
		std::error_code existsError;
		const bool bMiniatureExists = std::filesystem::is_regular_file(
			miniaturePath,
			existsError);
		if (bWasExpired || !bMiniatureExists || existsError)
		{
			ScheduleModelMiniature(
				modelAssetInfo,
				sourceRevision);
		}
	}
}

void ModelImporter::OnImportAsset(AssetInfoPtr)
{
}

Tasks::TaskPtr<bool> ModelImporter::ScheduleModelMiniature(
	ModelAssetInfoPtr assetInfo,
	const FileRevision& sourceRevision)
{
	if (assetInfo == nullptr || !assetInfo->GetFileId())
	{
		return {};
	}

	AssetRegistry* assetRegistry = App::GetSubmodule<AssetRegistry>();
	if (assetRegistry == nullptr)
	{
		return {};
	}

	FileRevision metadataRevision;
	if (!Utils::TryGetFileRevision(assetInfo->GetMetaFilepath(), metadataRevision))
	{
		SAILOR_LOG_ERROR(
			"Cannot capture model metadata revision for miniature generation: %s",
			assetInfo->GetMetaFilepath().c_str());
		return {};
	}

	const FileId fileId = assetInfo->GetFileId();
	const std::filesystem::path outputPath = ModelMiniature::GetCachePath(
		AssetRegistry::GetCacheFolder(),
		fileId);
	if (outputPath.empty())
	{
		SAILOR_LOG_ERROR(
			"Cannot resolve a safe miniature cache path for model FileId: %s",
			fileId.ToString().c_str());
		return {};
	}
	Tasks::TaskPtr<bool> miniatureTask;
	bool bMiniatureTaskNeedsRun = false;

	{
		std::lock_guard<std::mutex> lock(m_miniatureTasksMutex);
		ModelMiniatureTaskState* existingState = nullptr;
		if (m_miniatureTasks.Find(fileId, existingState) &&
			existingState != nullptr &&
			existingState->m_sourceRevision == sourceRevision &&
			existingState->m_metadataRevision == metadataRevision &&
			existingState->m_task)
		{
			std::error_code existsError;
			const bool bOutputExists = std::filesystem::is_regular_file(
				outputPath,
				existsError);
			const bool bTaskFinished =
				existingState->m_task->IsFinished();
			if (!bTaskFinished ||
				(existingState->m_task->GetResult() &&
					bOutputExists &&
					!existsError))
			{
				miniatureTask = existingState->m_task;
				bMiniatureTaskNeedsRun =
					!miniatureTask->IsStarted() &&
					!miniatureTask->IsFinished() &&
					!miniatureTask->IsInQueue();
			}
		}

		if (!miniatureTask)
		{
			const AssetRegistry::AssetProcessingToken processingToken =
				assetRegistry->BeginAssetProcessing(assetInfo);
			if (!processingToken)
			{
				return {};
			}
			if (processingToken.m_sourceRevision != sourceRevision)
			{
				assetRegistry->CompleteAssetProcessing(
					processingToken,
					false);
				SAILOR_LOG_ERROR(
					"Model source changed while generating dependent assets; retrying on the next scan: %s",
					assetInfo->GetAssetFilepath().c_str());
				return {};
			}
			const std::string assetFilepath = assetInfo->GetAssetFilepath();
			const float unitScale = assetInfo->GetUnitScale();
			const bool bShouldBatchByMaterial = assetInfo->ShouldBatchByMaterial();
			const TVector<FileId> defaultMaterials = assetInfo->GetDefaultMaterials();

			miniatureTask = Tasks::CreateTaskWithResult<bool>(
				"Generate model miniature",
				[
					this,
					fileId,
					assetFilepath,
					unitScale,
					bShouldBatchByMaterial,
					defaultMaterials,
					outputPath,
					processingToken
				]()
				{
					const bool bSucceeded = GenerateModelMiniature(
						fileId,
						assetFilepath,
						unitScale,
						bShouldBatchByMaterial,
						defaultMaterials,
						outputPath);
					if (AssetRegistry* currentRegistry = App::GetSubmodule<AssetRegistry>())
					{
						currentRegistry->CompleteAssetProcessing(
							processingToken,
							bSucceeded);
					}
					return bSucceeded;
				},
				EThreadType::Worker);

			if (m_lastMiniatureTask && !m_lastMiniatureTask->IsFinished())
			{
				miniatureTask->Join(m_lastMiniatureTask);
			}

			m_lastMiniatureTask = miniatureTask;
			m_miniatureTasks[fileId] = ModelMiniatureTaskState{
				processingToken
					? processingToken.m_sourceRevision
					: sourceRevision,
				metadataRevision,
				miniatureTask
			};
			bMiniatureTaskNeedsRun = true;
		}
	}

	assetRegistry->TrackScanProcessingTask(
		miniatureTask,
		bMiniatureTaskNeedsRun);
	return miniatureTask;
}

bool ModelImporter::GenerateModelMiniature(
	const FileId& fileId,
	const std::string& assetFilepath,
	float unitScale,
	bool bShouldBatchByMaterial,
	const TVector<FileId>& defaultMaterials,
	const std::filesystem::path& outputPath)
{
	Utils::Timer timer;
	timer.Start();

	TVector<MeshContext> parsedMeshes;
	TVector<glm::mat4> inverseBind;
	Math::AABB boundsAabb;
	Math::Sphere boundsSphere;
	if (!ImportModel(
			assetFilepath,
			unitScale,
			bShouldBatchByMaterial,
			parsedMeshes,
			boundsAabb,
			boundsSphere,
			inverseBind) ||
		parsedMeshes.Num() == 0 ||
		!boundsAabb.IsValid())
	{
		SAILOR_LOG_ERROR(
			"Cannot prepare model geometry for miniature: %s",
			assetFilepath.c_str());
		return false;
	}

	ObjectAllocatorPtr previewAllocator =
		ObjectAllocatorPtr::Make(EAllocationPolicy::SharedMemory_MultiThreaded);
	ModelPtr previewModel = ModelPtr::Make(previewAllocator, fileId);
	previewModel->m_boundsAabb = boundsAabb;
	previewModel->m_boundsSphere = boundsSphere;
	previewModel->m_inverseBind = std::move(inverseBind);
	previewModel->m_cpuMeshes.Reserve(parsedMeshes.Num());

	for (MeshContext& mesh : parsedMeshes)
	{
		Model::MeshCpuData cpuMesh{};
		cpuMesh.m_vertices = std::move(mesh.outVertices);
		cpuMesh.m_indices = std::move(mesh.outIndices);
		cpuMesh.m_bounds = mesh.bounds;
		cpuMesh.m_materialIndex = mesh.materialIndex;
		previewModel->m_cpuMeshes.Add(std::move(cpuMesh));
	}

	if (!previewModel->BuildBLAS())
	{
		SAILOR_LOG_ERROR(
			"Cannot build model BLAS for miniature: %s",
			assetFilepath.c_str());
		return false;
	}

	TVector<MaterialPtr> previewMaterials(defaultMaterials.Num());
	TMap<FileId, TexturePtr> previewTextures;
	MaterialImporter* materialImporter = App::GetSubmodule<MaterialImporter>();
	TextureImporter* textureImporter = App::GetSubmodule<TextureImporter>();
	if (materialImporter != nullptr && textureImporter != nullptr)
	{
		for (size_t i = 0; i < defaultMaterials.Num(); ++i)
		{
			const FileId materialFileId = defaultMaterials[i];
			const TSharedPtr<MaterialAsset> materialAsset =
				materialImporter->LoadMaterialAsset(materialFileId);
			if (!materialAsset)
			{
				SAILOR_LOG_ERROR(
					"Cannot load material %s for model miniature: %s",
					materialFileId.ToString().c_str(),
					assetFilepath.c_str());
				return false;
			}

			MaterialPtr material = MaterialPtr::Make(
				previewAllocator,
				materialFileId);
			material->SetRenderState(materialAsset->GetRenderState());
			for (const auto& uniform : materialAsset->GetUniformsVec4())
			{
				material->SetUniform(uniform.m_first, *uniform.m_second);
			}
			for (const auto& uniform : materialAsset->GetUniformsFloat())
			{
				material->SetUniform(uniform.m_first, *uniform.m_second);
			}
			for (const auto& sampler : materialAsset->GetSamplers())
			{
				TexturePtr texture;
				TexturePtr* cachedTexture = nullptr;
				if (previewTextures.Find(*sampler.m_second, cachedTexture) &&
					cachedTexture != nullptr)
				{
					texture = *cachedTexture;
				}
				else if (textureImporter->LoadTextureCpu_Immediate(
					*sampler.m_second,
					texture,
					ModelMiniature::TextureResolution))
				{
					previewTextures[*sampler.m_second] = texture;
				}

				if (!texture)
				{
					SAILOR_LOG_ERROR(
						"Cannot load texture %s for model miniature: %s",
						sampler.m_second->ToString().c_str(),
						assetFilepath.c_str());
					return false;
				}
				else
				{
					material->SetSampler(sampler.m_first, texture);
				}
			}
			previewMaterials[i] = std::move(material);
		}
	}

	Raytracing::PathTracer::TLASInstance instance{};
	instance.m_model = previewModel;
	instance.m_worldBounds = boundsAabb;
	instance.m_worldMatrix = glm::mat4(1.0f);
	instance.m_inverseWorldMatrix = glm::mat4(1.0f);
	instance.m_materialBaseOffset = 0;

	Raytracing::PathTracer pathTracer;
	if (!pathTracer.InitializeScene(
			TVector<Raytracing::PathTracer::TLASInstance>{ instance },
			previewMaterials,
			{}))
	{
		SAILOR_LOG_ERROR(
			"Cannot initialize path tracer for model miniature: %s",
			assetFilepath.c_str());
		return false;
	}

	Raytracing::PathTracer::Params params{};
	params.m_width = ModelMiniature::Resolution;
	params.m_height = ModelMiniature::Resolution;
	params.m_numSamples = 1;
	params.m_numAmbientSamples = 1;
	params.m_maxBounces = 1;
	params.m_msaa = 4;
	params.m_ambient = vec3(0.08f);
	params.m_rayBiasScale = 3e-4f;
	params.m_bRunTasksInline = true;
	if (!pathTracer.RenderPreparedScene(params) ||
		pathTracer.GetLastRenderedExtent() !=
			glm::uvec2(ModelMiniature::Resolution, ModelMiniature::Resolution))
	{
		SAILOR_LOG_ERROR(
			"Cannot render model miniature: %s",
			assetFilepath.c_str());
		return false;
	}

	TVector<uint8_t> png;
	if (!Raytracing::PathTracer::EncodePng(
			pathTracer.GetLastRenderedImage(),
			pathTracer.GetLastRenderedExtent(),
			png))
	{
		SAILOR_LOG_ERROR(
			"Cannot encode model miniature: %s",
			assetFilepath.c_str());
		return false;
	}

	std::string writeDiagnostic;
	if (!Workspace::AtomicReplaceWorkspaceCacheBinary(
			outputPath,
			png.GetData(),
			static_cast<uint64_t>(png.Num()),
			writeDiagnostic))
	{
		SAILOR_LOG_ERROR(
			"Cannot publish model miniature '%s': %s",
			outputPath.string().c_str(),
			writeDiagnostic.c_str());
		return false;
	}

	timer.Stop();
	SAILOR_LOG(
		"Generated model miniature '%s' in %.2f ms.",
		outputPath.string().c_str(),
		static_cast<double>(timer.ResultAccumulatedMs()));
	return true;
}

FileId CreateTextureAsset(const std::string& filepath,
	const std::string& glbFilename,
	uint32_t glbTextureIndex,
	bool bShouldGenerateMips = true,
	RHI::EFormat format = RHI::EFormat::R8G8B8A8_SRGB,
	RHI::ETextureClamping clamping = RHI::ETextureClamping::Repeat,
	RHI::ETextureFiltration filtration = RHI::ETextureFiltration::Linear,
	bool bShouldKeepCpuBuffers = false)
{
	AssetRegistry* assetRegistry = App::GetSubmodule<AssetRegistry>();
	if (assetRegistry == nullptr)
	{
		return FileId::Invalid;
	}

	FileId fileId;
	std::error_code existsError;
	const std::filesystem::file_status metadataStatus =
		std::filesystem::symlink_status(
		filepath,
		existsError);
	if (existsError == std::errc::no_such_file_or_directory ||
		existsError == std::errc::not_a_directory)
	{
		existsError.clear();
	}
	if (existsError)
	{
		SAILOR_LOG_ERROR(
			"Cannot inspect generated texture metadata path: %s",
			filepath.c_str());
		return FileId::Invalid;
	}
	const bool bMetadataExists = std::filesystem::exists(metadataStatus);
	if (bMetadataExists &&
		!std::filesystem::is_regular_file(metadataStatus))
	{
		SAILOR_LOG_ERROR(
			"Generated texture metadata path is not a regular file: %s",
			filepath.c_str());
		return FileId::Invalid;
	}
	if (bMetadataExists)
	{
		fileId = assetRegistry->RegisterGeneratedSecondaryAssetInfo(filepath);
		TextureAssetInfoPtr existingTextureInfo =
			assetRegistry->GetAssetInfoPtr<TextureAssetInfoPtr>(fileId);
		if (!fileId ||
			existingTextureInfo == nullptr ||
			existingTextureInfo->GetAssetFilename() != glbFilename)
		{
			SAILOR_LOG_ERROR(
				"Existing generated texture metadata is incompatible: %s",
				filepath.c_str());
			return FileId::Invalid;
		}
		return fileId;
	}
	else
	{
		fileId = FileId::CreateNewFileId();
	}

	YAML::Node newTexture = GeneratedModelAssetMetadata::CreateTexture(
		fileId,
		glbFilename,
		glbTextureIndex,
		bShouldGenerateMips,
		format,
		clamping,
		filtration,
		bShouldKeepCpuBuffers);

	std::ofstream assetFile(filepath);
	assetFile << newTexture;
	assetFile.close();
	if (!assetFile)
	{
		SAILOR_LOG_ERROR(
			"Cannot write generated texture metadata: %s",
			filepath.c_str());
		return FileId::Invalid;
	}

	if (assetRegistry->RegisterGeneratedSecondaryAssetInfo(filepath) != fileId)
	{
		SAILOR_LOG_ERROR(
			"Cannot register generated texture metadata for immediate model processing: %s",
			filepath.c_str());
		return FileId::Invalid;
	}

	return fileId;
}

FileId CreateAnimationAsset(const std::string& filepath,
	const std::string& glbFilename,
	uint32_t animationIndex,
	uint32_t skinIndex)
{
	FileId newFileId = FileId::CreateNewFileId();

	YAML::Node newAnimation = GeneratedModelAssetMetadata::CreateAnimation(
		newFileId,
		glbFilename,
		animationIndex,
		skinIndex);

	std::ofstream assetFile(filepath);
	assetFile << newAnimation;
	assetFile.close();

	return newFileId;
}

void ModelImporter::GenerateAnimationAssets(ModelAssetInfoPtr assetInfo)
{
	SAILOR_PROFILE_FUNCTION();

	tinygltf::Model gltfModel;
	tinygltf::TinyGLTF loader;
	std::string err, warn;

	const bool bIsGlb = Utils::GetFileExtension(assetInfo->GetAssetFilepath().c_str()) == "glb";
	const bool bGltfParsed = bIsGlb ?
		loader.LoadBinaryFromFile(&gltfModel, &err, &warn, assetInfo->GetAssetFilepath().c_str()) :
		loader.LoadASCIIFromFile(&gltfModel, &err, &warn, assetInfo->GetAssetFilepath().c_str());

	if (!bGltfParsed || gltfModel.animations.empty())
	{
		return;
	}

	const std::string animationsFolder = Utils::GetFileFolder(assetInfo->GetRelativeAssetFilepath());

	for (size_t i = 0; i < gltfModel.animations.size(); ++i)
	{
		const auto& anim = gltfModel.animations[i];
		std::string name = !anim.name.empty() ? anim.name : ("animation" + std::to_string(i));
		std::filesystem::path outputPath;
		if (!App::GetSubmodule<AssetRegistry>()->ResolveWorkspaceContentPathForWrite(
				animationsFolder + assetInfo->GetAssetFilename() + "_" + name + ".anim.asset",
				outputPath))
		{
			SAILOR_LOG_ERROR("Cannot resolve generated animation output for %s.", assetInfo->GetAssetFilepath().c_str());
			continue;
		}
		FileId id = CreateAnimationAsset(outputPath.string(),
			assetInfo->GetAssetFilename(), (uint32_t)i, 0);
		assetInfo->GetAnimations().Add(id);
	}
}

bool ModelImporter::GenerateMaterialAssets(ModelAssetInfoPtr assetInfo)
{
	SAILOR_PROFILE_FUNCTION();

	tinygltf::Model gltfModel;
	tinygltf::TinyGLTF loader;
	std::string err;
	std::string warn;

	const bool bIsGlb = Utils::GetFileExtension(assetInfo->GetAssetFilepath().c_str()) == "glb";
	const bool bGltfParsed = bIsGlb ?
		loader.LoadBinaryFromFile(&gltfModel, &err, &warn, assetInfo->GetAssetFilepath().c_str())
		: loader.LoadASCIIFromFile(&gltfModel, &err, &warn, assetInfo->GetAssetFilepath().c_str());

	if (!err.empty())
	{
		SAILOR_LOG_ERROR("Parsing gltf %s error: %s", assetInfo->GetAssetFilepath().c_str(), err.c_str());
	}

	if (!warn.empty())
	{
		SAILOR_LOG("Parsing gltf %s warning: %s", assetInfo->GetAssetFilepath().c_str(), warn.c_str());
	}

	if (!bGltfParsed)
	{
		return false;
	}

	const std::string texturesFolder = Utils::GetFileFolder(assetInfo->GetRelativeAssetFilepath());

	TVector<MaterialAsset::Data> materials(gltfModel.materials.size());
	TSet<std::string> materialNames;
	bool bGeneratedTexturesReady = true;

	for (size_t i = 0; i < gltfModel.materials.size(); ++i)
	{
		const auto& material = gltfModel.materials[i];

		MaterialAsset::Data& data = materials[i];
		const std::string materialBaseName =
			!material.name.empty()
				? material.name
				: ("material" + std::to_string(i));
		data.m_name = materialBaseName;
		for (size_t suffix = 1; !materialNames.Insert(data.m_name); ++suffix)
		{
			data.m_name =
				materialBaseName +
				"_" +
				std::to_string(suffix);
		}

		std::filesystem::path materialNamePath;
		if (!App::GetSubmodule<AssetRegistry>()->ResolveWorkspaceContentPathForWrite(
				texturesFolder + assetInfo->GetAssetFilename() + "_" + data.m_name,
				materialNamePath))
		{
			SAILOR_LOG_ERROR("Cannot resolve generated material output for %s.", assetInfo->GetAssetFilepath().c_str());
			return false;
		}
		const std::string materialName = materialNamePath.string();
		auto addTexture = [&](
			const std::string& sampler,
			const std::string& metadataPath,
			int32_t textureIndex,
			RHI::ETextureFormat format)
			{
				const FileId textureFileId = CreateTextureAsset(
					metadataPath,
					assetInfo->GetAssetFilename(),
					static_cast<uint32_t>(textureIndex),
					true,
					format,
					RHI::ETextureClamping::Repeat,
					RHI::ETextureFiltration::Linear,
					assetInfo->ShouldKeepCpuBuffers());
				if (textureFileId)
				{
					data.m_samplers.Add(sampler, textureFileId);
				}
				else
				{
					bGeneratedTexturesReady = false;
				}
			};

		if (material.pbrMetallicRoughness.baseColorTexture.index != -1)
		{
			addTexture(
				"baseColorSampler",
				materialName + "_baseColorTexture.png.asset",
				material.pbrMetallicRoughness.baseColorTexture.index,
				RHI::ETextureFormat::R8G8B8A8_SRGB);
		}

		if (material.normalTexture.index != -1)
		{
			addTexture(
				"normalSampler",
				materialName + "_normalTexture.png.asset",
				material.normalTexture.index,
				RHI::ETextureFormat::R8G8B8A8_UNORM);
		}

		if (material.emissiveTexture.index != -1)
		{
			addTexture(
				"emissiveSampler",
				materialName + "_emissionTexture.png.asset",
				material.emissiveTexture.index,
				RHI::ETextureFormat::R8G8B8A8_SRGB);
		}

		if (material.pbrMetallicRoughness.metallicRoughnessTexture.index != -1)
		{
			addTexture(
				"ormSampler",
				materialName + "_ormTexture.png.asset",
				material.pbrMetallicRoughness.metallicRoughnessTexture.index,
				RHI::ETextureFormat::R8G8B8A8_SRGB);
		}

		if (material.occlusionTexture.index != -1)
		{
			addTexture(
				"occlusionSampler",
				materialName + "_occlusionTexture.png.asset",
				material.occlusionTexture.index,
				RHI::ETextureFormat::R8G8B8A8_SRGB);
		}

		auto ccIt = material.extensions.find("KHR_materials_clearcoat");
		if (ccIt != material.extensions.end())
		{
			const tinygltf::Value& cc = ccIt->second;

			double ccFactor = 0.0;
			if (cc.Has("clearcoatFactor"))
				ccFactor = cc.Get("clearcoatFactor").GetNumberAsDouble();

			double ccRoughness = 0.0;
			if (cc.Has("clearcoatRoughnessFactor"))
				ccRoughness = cc.Get("clearcoatRoughnessFactor").GetNumberAsDouble();

			data.m_uniformsFloat.Add("material.clearcoatFactor", (float)ccFactor);
			data.m_uniformsFloat.Add("material.clearcoatRoughnessFactor", (float)ccRoughness);

			if (cc.Has("clearcoatTexture"))
			{
				const tinygltf::Value& tex = cc.Get("clearcoatTexture");
				if (tex.Has("index"))
				{
					int idx = tex.Get("index").Get<int>();
					if (idx != -1)
					{
						addTexture(
							"clearcoatSampler",
							materialName + "_clearcoatTexture.png.asset",
							idx,
							RHI::ETextureFormat::R8G8B8A8_SRGB);
					}
				}
			}

			if (cc.Has("clearcoatRoughnessTexture"))
			{
				const tinygltf::Value& tex = cc.Get("clearcoatRoughnessTexture");
				if (tex.Has("index"))
				{
					int idx = tex.Get("index").Get<int>();
					if (idx != -1)
					{
						addTexture(
							"clearcoatRoughnessSampler",
							materialName + "_clearcoatRoughnessTexture.png.asset",
							idx,
							RHI::ETextureFormat::R8G8B8A8_SRGB);
					}
				}
			}

			if (cc.Has("clearcoatNormalTexture"))
			{
				const tinygltf::Value& tex = cc.Get("clearcoatNormalTexture");
				double scale = 1.0;
				if (tex.Has("scale"))
					scale = tex.Get("scale").GetNumberAsDouble();

				if (tex.Has("index"))
				{
					int idx = tex.Get("index").Get<int>();
					if (idx != -1)
					{
						addTexture(
							"clearcoatNormalSampler",
							materialName + "_clearcoatNormalTexture.png.asset",
							idx,
							RHI::ETextureFormat::R8G8B8A8_UNORM);
					}
				}
				data.m_uniformsFloat.Add("material.clearcoatNormalScale", (float)scale);
			}

			data.m_shaderDefines.Add("CLEAR_COAT");
		}

		auto sheenIt = material.extensions.find("KHR_materials_sheen");
		if (sheenIt != material.extensions.end())
		{
			const tinygltf::Value& sheen = sheenIt->second;

			glm::vec3 color = glm::vec3(0.0f);
			if (sheen.Has("sheenColorFactor"))
			{
				auto arr = sheen.Get("sheenColorFactor").Get<tinygltf::Value::Array>();
				color = glm::vec3((float)arr[0].GetNumberAsDouble(), (float)arr[1].GetNumberAsDouble(), (float)arr[2].GetNumberAsDouble());
			}

			double roughness = 0.0;
			if (sheen.Has("sheenRoughnessFactor"))
			{
				roughness = sheen.Get("sheenRoughnessFactor").GetNumberAsDouble();
			}

			data.m_uniformsVec4.Add("material.sheenColorFactor", glm::vec4(color, 0.0f));
			data.m_uniformsFloat.Add("material.sheenRoughnessFactor", (float)roughness);

			if (sheen.Has("sheenColorTexture"))
			{
				const tinygltf::Value& tex = sheen.Get("sheenColorTexture");
				if (tex.Has("index"))
				{
					int idx = tex.Get("index").Get<int>();
					if (idx != -1)
					{
						addTexture(
							"sheenColorSampler",
							materialName + "_sheenColorTexture.png.asset",
							idx,
							RHI::ETextureFormat::R8G8B8A8_SRGB);
					}
				}
			}

			if (sheen.Has("sheenRoughnessTexture"))
			{
				const tinygltf::Value& tex = sheen.Get("sheenRoughnessTexture");
				if (tex.Has("index"))
				{
					int idx = tex.Get("index").Get<int>();
					if (idx != -1)
					{
						addTexture(
							"sheenRoughnessSampler",
							materialName + "_sheenRoughnessTexture.png.asset",
							idx,
							RHI::ETextureFormat::R8G8B8A8_SRGB);
					}
				}
			}

			data.m_shaderDefines.Add("SHEEN");
		}

		const vec4 baseColor = vec4((float)material.pbrMetallicRoughness.baseColorFactor[0],
			(float)material.pbrMetallicRoughness.baseColorFactor[1],
			(float)material.pbrMetallicRoughness.baseColorFactor[2],
			(float)material.pbrMetallicRoughness.baseColorFactor[3]);

		const vec4 emissiveFactor = vec4((float)material.emissiveFactor[0], (float)material.emissiveFactor[1], (float)material.emissiveFactor[2], 0.0f);

		data.m_uniformsVec4.Add("material.baseColorFactor", baseColor);
		data.m_uniformsVec4.Add("material.emissiveFactor", emissiveFactor);

		data.m_uniformsFloat.Add("material.roughnessFactor", (float)material.pbrMetallicRoughness.roughnessFactor);
		data.m_uniformsFloat.Add("material.metallicFactor", (float)material.pbrMetallicRoughness.metallicFactor);
		data.m_uniformsFloat.Add("material.normalScale", (float)material.normalTexture.scale);
		data.m_uniformsFloat.Add("material.alphaCutoff", (float)material.alphaCutoff);
		data.m_uniformsFloat.Add("material.occlusionStrength", (float)material.occlusionTexture.strength);

		const bool bIsTransparent = material.alphaMode == "BLEND";
		const bool bIsMasked = material.alphaMode == "MASK";

		data.m_renderQueue = bIsTransparent ? "Transparent" : "Opaque";

		if (bIsMasked)
		{
			data.m_renderQueue = "Masked";
			data.m_shaderDefines.Add("ALPHA_CUTOUT");
		}

		data.m_renderState = RHI::RenderState(true,
			!bIsTransparent,
			0.0f, bIsMasked,
			material.doubleSided ? RHI::ECullMode::None : RHI::ECullMode::Back,
			RHI::EBlendMode::None,
			RHI::EFillMode::Fill);

		data.m_shader = App::GetSubmodule<AssetRegistry>()->GetOrLoadFile("Shaders/Standard_glTF.shader");
	}
	if (!bGeneratedTexturesReady)
	{
		return false;
	}

	TVector<FileId> materialFiles;

	for (const auto& material : materials)
	{
		std::filesystem::path materialsFolder;
		if (!App::GetSubmodule<AssetRegistry>()->ResolveWorkspaceContentPathForWrite(
				texturesFolder + "materials",
				materialsFolder))
		{
			SAILOR_LOG_ERROR("Cannot resolve generated materials folder for %s.", assetInfo->GetAssetFilepath().c_str());
			return false;
		}
		std::filesystem::create_directories(materialsFolder);

		FileId materialFileId = App::GetSubmodule<MaterialImporter>()->CreateMaterialAsset(
			(materialsFolder / (material.m_name + ".mat")).string(),
			material);
		if (!materialFileId)
		{
			return false;
		}
		materialFiles.Add(materialFileId);
	}

	if (assetInfo->ShouldBatchByMaterial())
	{
		for (const auto& materialFileId : materialFiles)
		{
			assetInfo->GetDefaultMaterials().Add(materialFileId);
		}
	}
	else
	{
		for (const auto& mesh : gltfModel.meshes)
		{
			for (const auto& primitive : mesh.primitives)
			{
				assetInfo->GetDefaultMaterials().Add(materialFiles[primitive.material]);
			}
		}
	}
	return true;
}

Tasks::TaskPtr<ModelPtr> ModelImporter::LoadModel(FileId uid, ModelPtr& outModel)
{
	SAILOR_PROFILE_FUNCTION();
	ModelAssetInfoPtr pAssetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<ModelAssetInfoPtr>(uid);

	// Check promises first
	auto& promise = m_promises.At_Lock(uid, nullptr);
	auto& loadedModel = m_loadedModels.At_Lock(uid, ModelPtr());

	// Check loaded assets
	if (loadedModel)
	{
		const bool bNeedCpuBuffers = pAssetInfo && pAssetInfo->ShouldKeepCpuBuffers() && !loadedModel->HasCpuMeshes();
		if (bNeedCpuBuffers && !promise)
		{
			loadedModel = nullptr;
		}
		else
		{
			outModel = loadedModel;
			auto res = promise ? promise : Tasks::TaskPtr<ModelPtr>::Make(outModel);

			m_loadedModels.Unlock(uid);
			m_promises.Unlock(uid);

			return res;
		}
	}

	// There is no promise, we need to load model
	if (pAssetInfo)
	{
		SAILOR_PROFILE_TEXT(pAssetInfo->GetAssetFilepath().c_str());

		ModelPtr pModel = ModelPtr::Make(m_allocator, uid);

		// The way to drop qualifiers inside lambda
		auto& boundsSphere = pModel->m_boundsSphere;
		auto& boundsAabb = pModel->m_boundsAabb;

		struct Data
		{
			TVector<MeshContext> m_parsedMeshes;
			TVector<glm::mat4> m_inverseBind;
			bool m_bIsImported = false;
			bool m_bShouldKeepCpuBuffers = false;
			bool m_bShouldGenerateBLAS = false;
		};

		promise = Tasks::CreateTaskWithResult<TSharedPtr<Data>>("Load model",
			[pAssetInfo, &boundsAabb, &boundsSphere]()
			{
				TSharedPtr<Data> pData = TSharedPtr<Data>::Make();
				pData->m_bShouldKeepCpuBuffers = pAssetInfo->ShouldKeepCpuBuffers();
				pData->m_bShouldGenerateBLAS = pAssetInfo->ShouldGenerateBLAS();
				pData->m_bIsImported = ImportModel(pAssetInfo, pData->m_parsedMeshes, boundsAabb, boundsSphere, pData->m_inverseBind);
				return pData;
			})->Then<ModelPtr>([pModel](TSharedPtr<Data> pData) mutable
				{
					if (pData->m_bIsImported)
					{
						pModel->m_meshes.Clear();
						pModel->m_cpuMeshes.Clear();
						pModel->m_meshes.Reserve(pData->m_parsedMeshes.Num());
						if (pData->m_bShouldKeepCpuBuffers || pData->m_bShouldGenerateBLAS)
						{
							pModel->m_cpuMeshes.Reserve(pData->m_parsedMeshes.Num());
						}

						for (auto& mesh : pData->m_parsedMeshes)
						{
							RHI::RHIMeshPtr pMesh = RHI::Renderer::GetDriver()->CreateMesh();
							pMesh->m_vertexDescription = RHI::Renderer::GetDriver()->GetOrAddVertexDescription<RHI::VertexP3N3T3B3UV2C4I4W4>();
							pMesh->m_bounds = mesh.bounds;
							RHI::Renderer::GetDriver()->UpdateMesh(pMesh,
								mesh.outVertices.GetData(), sizeof(RHI::VertexP3N3T3B3UV2C4I4W4) * mesh.outVertices.Num(),
								mesh.outIndices.GetData(), sizeof(uint32_t) * mesh.outIndices.Num());

							pModel->m_meshes.Emplace(pMesh);

							if (pData->m_bShouldKeepCpuBuffers || pData->m_bShouldGenerateBLAS)
							{
								Model::MeshCpuData cpuMesh{};
								cpuMesh.m_vertices = std::move(mesh.outVertices);
								cpuMesh.m_indices = std::move(mesh.outIndices);
								cpuMesh.m_bounds = mesh.bounds;
								cpuMesh.m_materialIndex = mesh.materialIndex;
								pModel->m_cpuMeshes.Add(std::move(cpuMesh));
							}
						}

						pModel->m_inverseBind = std::move(pData->m_inverseBind);
						pModel->ProceedCpuMeshes(pData->m_bShouldGenerateBLAS, pData->m_bShouldKeepCpuBuffers);
						pModel->Flush();
					}
					return pModel;
				}, "Update RHI Meshes", EThreadType::RHI)->ToTaskWithResult();

			outModel = loadedModel = pModel;
			promise->Run();

			m_loadedModels.Unlock(uid);
			m_promises.Unlock(uid);

			return promise;
	}

	outModel = nullptr;
	m_loadedModels.Unlock(uid);
	m_promises.Unlock(uid);

	return Tasks::TaskPtr<ModelPtr>();
}

bool ModelImporter::LoadModel_Immediate(FileId uid, ModelPtr& outModel)
{
	SAILOR_PROFILE_FUNCTION();

	auto task = LoadModel(uid, outModel);
	task->Wait();
	return task->GetResult().IsValid();
}

void CalculateTangentBitangent(vec3& outTangent, vec3& outBitangent, const vec3* vert, const vec2* uv)
{
	outTangent = vec3(0.0f);
	outBitangent = vec3(0.0f);

	vec3 edge1 = vert[1] - vert[0];
	vec3 edge2 = vert[2] - vert[0];

	vec2 deltaUV1 = uv[1] - uv[0];
	vec2 deltaUV2 = uv[2] - uv[0];

	float denominator = deltaUV1.x * deltaUV2.y - deltaUV2.x * deltaUV1.y;
	if (abs(denominator) < 1e-6f)
	{
		const vec3 normal = glm::cross(edge1, edge2);
		const vec3 tangentFallback = glm::cross(
			glm::abs(normal.y) < 0.999f
				? vec3(0.0f, 1.0f, 0.0f)
				: vec3(1.0f, 0.0f, 0.0f),
			normal);
		outTangent = NormalizeOrFallback(tangentFallback, vec3(1.0f, 0.0f, 0.0f));
		outBitangent = NormalizeOrFallback(
			glm::cross(normal, outTangent),
			vec3(0.0f, 0.0f, 1.0f));
		return;
	}

	float f = 1.0f / denominator;

	outTangent = vec3(
		f * (deltaUV2.y * edge1.x - deltaUV1.y * edge2.x),
		f * (deltaUV2.y * edge1.y - deltaUV1.y * edge2.y),
		f * (deltaUV2.y * edge1.z - deltaUV1.y * edge2.z)
	);

	vec3 normal = cross(edge1, edge2);
	outBitangent = normalize(cross(normal, outTangent));
	outTangent = normalize(outTangent);
}
static glm::vec3 CalculateNormal(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2)
{
	return glm::normalize(glm::cross(v1 - v0, v2 - v0));
}

static void GenerateTangents(ModelImporter::MeshContext& meshContext,
	uint32_t vertexOffset,
	uint32_t vertexCount,
	uint32_t indexOffset,
	uint32_t indexCount)
{
	TVector<glm::vec3> tangents(vertexCount, glm::vec3(0.0f));
	TVector<glm::vec3> bitangents(vertexCount, glm::vec3(0.0f));

	auto processTriangle = [&](uint32_t idx0, uint32_t idx1, uint32_t idx2)
		{
			glm::vec3 verts[3] = {
					meshContext.outVertices[idx0].m_position,
					meshContext.outVertices[idx1].m_position,
					meshContext.outVertices[idx2].m_position };

			glm::vec2 uvs[3] = {
					meshContext.outVertices[idx0].m_texcoord,
					meshContext.outVertices[idx1].m_texcoord,
					meshContext.outVertices[idx2].m_texcoord };

			glm::vec3 t, b;
			CalculateTangentBitangent(t, b, verts, uvs);

			tangents[idx0 - vertexOffset] += t;
			tangents[idx1 - vertexOffset] += t;
			tangents[idx2 - vertexOffset] += t;

			bitangents[idx0 - vertexOffset] += b;
			bitangents[idx1 - vertexOffset] += b;
			bitangents[idx2 - vertexOffset] += b;
		};

	if (indexCount > 0 && meshContext.outIndices.Num() > 0)
	{
		for (uint32_t i = 0; i + 2 < indexCount; i += 3)
		{
			uint32_t idx0 = meshContext.outIndices[indexOffset + i];
			uint32_t idx1 = meshContext.outIndices[indexOffset + i + 1];
			uint32_t idx2 = meshContext.outIndices[indexOffset + i + 2];
			processTriangle(idx0, idx1, idx2);
		}
	}
	else
	{
		for (uint32_t i = 0; i + 2 < vertexCount; i += 3)
		{
			uint32_t idx0 = vertexOffset + i;
			uint32_t idx1 = vertexOffset + i + 1;
			uint32_t idx2 = vertexOffset + i + 2;
			processTriangle(idx0, idx1, idx2);
		}
	}

	for (uint32_t i = 0; i < vertexCount; ++i)
	{
		auto& vertex = meshContext.outVertices[vertexOffset + i];
		vec3 safeNormal;
		BuildSafeBasis(
			vertex.m_normal,
			tangents[i],
			bitangents[i],
			vertex.m_normal,
			safeNormal,
			vertex.m_tangent,
			vertex.m_bitangent);
	}
}

static void GenerateNormals(ModelImporter::MeshContext& meshContext,
	uint32_t vertexOffset,
	uint32_t vertexCount,
	uint32_t indexOffset,
	uint32_t indexCount)
{
	TVector<glm::vec3> normals(vertexCount, glm::vec3(0.0f));

	if (indexCount > 0 && meshContext.outIndices.Num() > 0)
	{
		for (uint32_t i = 0; i + 2 < indexCount; i += 3)
		{
			uint32_t idx0 = meshContext.outIndices[indexOffset + i];
			uint32_t idx1 = meshContext.outIndices[indexOffset + i + 1];
			uint32_t idx2 = meshContext.outIndices[indexOffset + i + 2];

			glm::vec3 normal = CalculateNormal(meshContext.outVertices[idx0].m_position, meshContext.outVertices[idx1].m_position, meshContext.outVertices[idx2].m_position);

			normals[idx0 - vertexOffset] += normal;
			normals[idx1 - vertexOffset] += normal;
			normals[idx2 - vertexOffset] += normal;
		}
	}
	else
	{
		for (uint32_t i = 0; i + 2 < vertexCount; i += 3)
		{
			uint32_t idx0 = vertexOffset + i;
			uint32_t idx1 = vertexOffset + i + 1;
			uint32_t idx2 = vertexOffset + i + 2;

			glm::vec3 normal = CalculateNormal(meshContext.outVertices[idx0].m_position, meshContext.outVertices[idx1].m_position, meshContext.outVertices[idx2].m_position);

			normals[i] += normal;
			normals[i + 1] += normal;
			normals[i + 2] += normal;
		}
	}

	for (uint32_t i = 0; i < vertexCount; ++i)
	{
		meshContext.outVertices[vertexOffset + i].m_normal = glm::normalize(normals[i]);
	}
}

bool ModelImporter::ImportModel(ModelAssetInfoPtr assetInfo, TVector<MeshContext>& outParsedMeshes, Math::AABB& outBoundsAabb, Math::Sphere& outBoundsSphere, TVector<glm::mat4>& outInverseBind)
{
	if (assetInfo == nullptr)
	{
		return false;
	}

	return ImportModel(
		assetInfo->GetAssetFilepath(),
		assetInfo->GetUnitScale(),
		assetInfo->ShouldBatchByMaterial(),
		outParsedMeshes,
		outBoundsAabb,
		outBoundsSphere,
		outInverseBind);
}

bool ModelImporter::ImportModel(
	const std::string& assetFilepath,
	float unitScale,
	bool bShouldBatchByMaterial,
	TVector<MeshContext>& outParsedMeshes,
	Math::AABB& outBoundsAabb,
	Math::Sphere& outBoundsSphere,
	TVector<glm::mat4>& outInverseBind)
{
	tinygltf::Model gltfModel;
	tinygltf::TinyGLTF loader;
	std::string err;
	std::string warn;

	const bool bGltfParsed = (Utils::GetFileExtension(assetFilepath.c_str()) == "glb") ?
		loader.LoadBinaryFromFile(&gltfModel, &err, &warn, assetFilepath.c_str())
		: loader.LoadASCIIFromFile(&gltfModel, &err, &warn, assetFilepath.c_str());

	if (!err.empty())
	{
		SAILOR_LOG_ERROR("Parsing gltf %s error: %s", assetFilepath.c_str(), err.c_str());
	}

	if (!warn.empty())
	{
		SAILOR_LOG("Parsing gltf %s warning: %s", assetFilepath.c_str(), warn.c_str());
	}

	if (!bGltfParsed)
	{
		return false;
	}

	outBoundsAabb.m_max = glm::vec3(std::numeric_limits<float>::lowest());
	outBoundsAabb.m_min = glm::vec3(std::numeric_limits<float>::max());

	outInverseBind.Clear();
	if (!gltfModel.skins.empty())
	{
		const auto& gltfSkin = gltfModel.skins[0];
		size_t numBones = gltfSkin.joints.size();
		outInverseBind.Resize(numBones);

		if (gltfSkin.inverseBindMatrices >= 0)
		{
			const auto& accessor = gltfModel.accessors[gltfSkin.inverseBindMatrices];
			const auto& view = gltfModel.bufferViews[accessor.bufferView];
			const float* data = reinterpret_cast<const float*>(&gltfModel.buffers[view.buffer].data[view.byteOffset + accessor.byteOffset]);
			for (size_t i = 0; i < numBones; ++i)
			{
				outInverseBind[i] = glm::make_mat4(data + i * 16);
			}
		}
		else
		{
			for (size_t i = 0; i < numBones; ++i) outInverseBind[i] = glm::mat4(1.0f);
		}
	}

	// At least one batch
	TVector<MeshContext> batchedMeshContexts(std::max<size_t>(1, gltfModel.materials.size()));

	for (const auto& mesh : gltfModel.meshes)
	{
		for (const auto& primitive : mesh.primitives)
		{
			MeshContext* pMeshContext = nullptr;
			uint32_t startIndex = 0;
			uint32_t indicesStart = 0;

			if (bShouldBatchByMaterial)
			{
				pMeshContext = &batchedMeshContexts[std::max(primitive.material, 0)];
				startIndex = (uint32_t)pMeshContext->outVertices.Num();
				indicesStart = (uint32_t)pMeshContext->outIndices.Num();
				pMeshContext->materialIndex = primitive.material;
			}
			else
			{
				outParsedMeshes.Add(MeshContext());
				pMeshContext = &(*outParsedMeshes.Last());
				indicesStart = (uint32_t)pMeshContext->outIndices.Num();
				pMeshContext->materialIndex = primitive.material;
			}

			const tinygltf::Accessor& posAccessor = gltfModel.accessors[primitive.attributes.find("POSITION")->second];
			const tinygltf::BufferView& posView = gltfModel.bufferViews[std::max(0, posAccessor.bufferView)];
			const float* posData = reinterpret_cast<const float*>(&gltfModel.buffers[posView.buffer].data[posView.byteOffset + posAccessor.byteOffset]);


			const tinygltf::Accessor* normAccessor = nullptr;
			const tinygltf::BufferView* normView = nullptr;
			const float* normData = nullptr;

			if (primitive.attributes.find("NORMAL") != primitive.attributes.end())
			{
				normAccessor = &gltfModel.accessors[primitive.attributes.find("NORMAL")->second];
				normView = &gltfModel.bufferViews[(std::max)(0, normAccessor->bufferView)];
				normData = reinterpret_cast<const float*>(&gltfModel.buffers[normView->buffer].data[normView->byteOffset + normAccessor->byteOffset]);
			}

			const bool bGenerateNormals = normData == nullptr;

			const tinygltf::Accessor* texAccessor = nullptr;
			const tinygltf::BufferView* texView = nullptr;
			const float* texData = nullptr;

			if (primitive.attributes.find("TEXCOORD_0") != primitive.attributes.end())
			{
				texAccessor = &gltfModel.accessors[primitive.attributes.find("TEXCOORD_0")->second];
				texView = &gltfModel.bufferViews[std::max(0, texAccessor->bufferView)];
				texData = reinterpret_cast<const float*>(&gltfModel.buffers[texView->buffer].data[texView->byteOffset + texAccessor->byteOffset]);
			}

			const tinygltf::Accessor* tanAccessor = nullptr;
			const tinygltf::BufferView* tanView = nullptr;
			const float* tanData = nullptr;

			if (primitive.attributes.find("TANGENT") != primitive.attributes.end())
			{
				tanAccessor = &gltfModel.accessors[primitive.attributes.find("TANGENT")->second];
				tanView = &gltfModel.bufferViews[std::max(0, tanAccessor->bufferView)];
				tanData = reinterpret_cast<const float*>(&gltfModel.buffers[tanView->buffer].data[tanView->byteOffset + tanAccessor->byteOffset]);
			}

			const bool bGenerateTangents = tanData == nullptr;

			const tinygltf::Accessor* colAccessor = nullptr;
			const tinygltf::BufferView* colView = nullptr;
			const float* colData = nullptr;

			const tinygltf::Accessor* jointsAccessor = nullptr;
			const tinygltf::BufferView* jointsView = nullptr;
			const unsigned char* jointsData8 = nullptr;
			const unsigned short* jointsData16 = nullptr;
			const unsigned int* jointsData32 = nullptr;

			const tinygltf::Accessor* weightsAccessor = nullptr;
			const tinygltf::BufferView* weightsView = nullptr;
			const float* weightsDataF = nullptr;
			const unsigned char* weightsData8 = nullptr;
			const unsigned short* weightsData16 = nullptr;

			if (primitive.attributes.find("COLOR_0") != primitive.attributes.end())
			{
				colAccessor = &gltfModel.accessors[primitive.attributes.find("COLOR_0")->second];
				colView = &gltfModel.bufferViews[std::max(0, colAccessor->bufferView)];
				colData = reinterpret_cast<const float*>(&gltfModel.buffers[colView->buffer].data[colView->byteOffset + colAccessor->byteOffset]);
			}

			if (primitive.attributes.find("JOINTS_0") != primitive.attributes.end())
			{
				jointsAccessor = &gltfModel.accessors[primitive.attributes.find("JOINTS_0")->second];
				jointsView = &gltfModel.bufferViews[std::max(0, jointsAccessor->bufferView)];
				const unsigned char* ptr = reinterpret_cast<const unsigned char*>(&gltfModel.buffers[jointsView->buffer].data[jointsView->byteOffset + jointsAccessor->byteOffset]);
				if (jointsAccessor->componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
				{
					jointsData8 = ptr;
				}
				else if (jointsAccessor->componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
				{
					jointsData16 = reinterpret_cast<const unsigned short*>(ptr);
				}
				else
				{
					jointsData32 = reinterpret_cast<const unsigned int*>(ptr);
				}
			}

			if (primitive.attributes.find("WEIGHTS_0") != primitive.attributes.end())
			{
				weightsAccessor = &gltfModel.accessors[primitive.attributes.find("WEIGHTS_0")->second];
				weightsView = &gltfModel.bufferViews[std::max(0, weightsAccessor->bufferView)];
				const unsigned char* ptr = reinterpret_cast<const unsigned char*>(&gltfModel.buffers[weightsView->buffer].data[weightsView->byteOffset + weightsAccessor->byteOffset]);
				if (weightsAccessor->componentType == TINYGLTF_COMPONENT_TYPE_FLOAT)
				{
					weightsDataF = reinterpret_cast<const float*>(ptr);
				}
				else if (weightsAccessor->componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
				{
					weightsData8 = ptr;
				}
				else if (weightsAccessor->componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
				{
					weightsData16 = reinterpret_cast<const unsigned short*>(ptr);
				}
			}

			const uint32_t colSize = (colAccessor && colAccessor->type == TINYGLTF_PARAMETER_TYPE_FLOAT_VEC3) ? 3 : 4;

			for (size_t i = 0; i < posAccessor.count; ++i)
			{
				Sailor::RHI::VertexP3N3T3B3UV2C4I4W4 vertex{};
				vertex.m_boneIds = glm::ivec4(0);
				vertex.m_boneWeights = glm::vec4(0.0f);
				vertex.m_position = glm::make_vec3(posData + i * 3) * unitScale;

				if (normData)
				{
					vertex.m_normal = glm::make_vec3(normData + i * 3);
				}
				else
				{
					vertex.m_normal = glm::vec3(0.0f);
				}

				if (colData)
				{
					vertex.m_color.x = colData[i * colSize];
					vertex.m_color.y = colData[i * colSize + 1];
					vertex.m_color.z = colData[i * colSize + 2];
					vertex.m_color.w = colSize == 4 ? colData[i * 3 + 3] : 1.0f;
				}
				else
				{
					vertex.m_color = glm::vec4(1.0f);
				}

				if (texData)
				{
					vertex.m_texcoord = glm::make_vec2(texData + i * 2);
				}

				if (tanData)
				{
					vertex.m_tangent = glm::make_vec3(tanData + i * 3);
				}
				else
				{
					vertex.m_tangent = glm::vec3(0.0f);
				}

				vertex.m_bitangent = glm::vec3(0.0f);

				if (jointsAccessor)
				{
					if (jointsData8)
					{
						const unsigned char* d = jointsData8 + i * 4;
						vertex.m_boneIds = glm::ivec4(d[0], d[1], d[2], d[3]);
					}
					else if (jointsData16)
					{
						const unsigned short* d = jointsData16 + i * 4;
						vertex.m_boneIds = glm::ivec4(d[0], d[1], d[2], d[3]);
					}
					else if (jointsData32)
					{
						const unsigned int* d = jointsData32 + i * 4;
						vertex.m_boneIds = glm::ivec4(d[0], d[1], d[2], d[3]);
					}
				}

				if (weightsAccessor)
				{
					if (weightsDataF)
					{
						vertex.m_boneWeights = glm::make_vec4(weightsDataF + i * 4);
					}
					else if (weightsData8)
					{
						const unsigned char* d = weightsData8 + i * 4;
						vertex.m_boneWeights = glm::vec4(d[0], d[1], d[2], d[3]) / 255.0f;
					}
					else if (weightsData16)
					{
						const unsigned short* d = weightsData16 + i * 4;
						vertex.m_boneWeights = glm::vec4(d[0], d[1], d[2], d[3]) / 65535.0f;
					}
				}

				pMeshContext->outVertices.Add(vertex);
				outBoundsAabb.Extend(vertex.m_position);
				pMeshContext->bounds.Extend(vertex.m_position);
			}

			uint32_t indexCount = 0;
			if (primitive.indices >= 0)
			{
				const tinygltf::Accessor& indexAccessor = gltfModel.accessors[primitive.indices];
				const tinygltf::BufferView& indexView = gltfModel.bufferViews[std::max(0, indexAccessor.bufferView)];

				for (size_t i = 0; i < indexAccessor.count; ++i)
				{
					uint32_t index = indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT ?
						(uint32_t)(reinterpret_cast<const uint16_t*>(&gltfModel.buffers[indexView.buffer].data[indexView.byteOffset + indexAccessor.byteOffset])[i])
						: reinterpret_cast<const uint32_t*>(&gltfModel.buffers[indexView.buffer].data[indexView.byteOffset + indexAccessor.byteOffset])[i];

					pMeshContext->outIndices.Add(index + startIndex);
				}
				indexCount = (uint32_t)indexAccessor.count;
			}
			else
			{
				// Always generate the index buffer
				// Assume TRIANGLES mode
				for (uint32_t i = 0; i < posAccessor.count; ++i)
				{
					pMeshContext->outIndices.Add(startIndex + i);
				}
				indexCount = (uint32_t)posAccessor.count;
			}

			if (bGenerateNormals)
			{
				GenerateNormals(*pMeshContext, startIndex, (uint32_t)posAccessor.count, indicesStart, indexCount);
			}

			if (bGenerateTangents || bGenerateNormals)
			{
				GenerateTangents(*pMeshContext, startIndex, (uint32_t)posAccessor.count, indicesStart, indexCount);
			}
			else
			{
				for (size_t i = 0; i < posAccessor.count; ++i)
				{
					auto& v = pMeshContext->outVertices[startIndex + i];
					v.m_bitangent = glm::cross(v.m_normal, v.m_tangent);
				}
			}
		}
	}

	if (bShouldBatchByMaterial)
	{
		for (auto& meshContext : batchedMeshContexts)
		{
			outParsedMeshes.Emplace(meshContext);
		}
	}

	outBoundsSphere.m_center = 0.5f * (outBoundsAabb.m_min + outBoundsAabb.m_max);
	outBoundsSphere.m_radius = glm::distance(outBoundsAabb.m_max, outBoundsSphere.m_center);

	return true;
}

Tasks::TaskPtr<bool> ModelImporter::LoadDefaultMaterials(FileId uid, TVector<MaterialPtr>& outMaterials)
{
	outMaterials.Clear();

	if (ModelAssetInfoPtr modelInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<ModelAssetInfoPtr>(uid))
	{
		Tasks::TaskPtr<bool> loadingFinished = Tasks::CreateTaskWithResult<bool>("Load Default Materials", []() { return true; });

		for (auto& assetInfo : modelInfo->GetDefaultMaterials())
		{
			MaterialPtr material;
			Tasks::ITaskPtr loadMaterial;
			if (assetInfo && (loadMaterial = App::GetSubmodule<MaterialImporter>()->LoadMaterial(assetInfo, material)))
			{
				if (material)
				{
					outMaterials.Add(material);
					//TODO: Add hot reloading dependency
					loadingFinished->Join(loadMaterial);
				}
				else
				{
					//TODO: push missing material
				}
			}
		}

		App::GetSubmodule<Tasks::Scheduler>()->Run(loadingFinished);
		return loadingFinished;
	}

	return Tasks::TaskPtr<bool>::Make(false);
}

bool ModelImporter::LoadAsset(FileId uid, TObjectPtr<Object>& out, bool bImmediate)
{
	ModelPtr outModel;
	if (bImmediate)
	{
		bool bRes = LoadModel_Immediate(uid, outModel);
		out = outModel;
		return bRes;
	}

	LoadModel(uid, outModel);
	out = outModel;
	return true;
}

void ModelImporter::CollectGarbage()
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

	{
		std::lock_guard<std::mutex> lock(m_miniatureTasksMutex);
		TVector<FileId> finishedMiniatures;
		for (const auto& miniature : m_miniatureTasks)
		{
			if (miniature.m_second->m_task &&
				miniature.m_second->m_task->IsFinished())
			{
				finishedMiniatures.Add(miniature.m_first);
			}
		}
		for (const FileId& fileId : finishedMiniatures)
		{
			m_miniatureTasks.Remove(fileId);
		}
		if (m_lastMiniatureTask && m_lastMiniatureTask->IsFinished())
		{
			m_lastMiniatureTask.Clear();
		}
	}
}
