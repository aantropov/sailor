#include "ModelImporter.h"
#include "AssetRegistry/FileId.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "ModelAssetInfo.h"
#include "Core/Utils.h"
#include "RHI/VertexDescription.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>
#include "RHI/Types.h"
#include "RHI/Renderer.h"
#include "Memory/ObjectAllocator.hpp"

//Assimp
#include "assimp/scene.h"
#include "assimp/postprocess.h"
#include "assimp/Importer.hpp"
#include "assimp/DefaultLogger.hpp"
#include "assimp/LogStream.hpp"

#include "nlohmann_json/include/nlohmann/json.hpp"
#include "Tasks/Scheduler.h"

using namespace Sailor;

namespace {
	const unsigned int DefaultImportFlags_Assimp =
		aiProcess_CalcTangentSpace |
		aiProcess_Triangulate |
		aiProcess_FlipUVs |
		aiProcess_SortByPType |
		aiProcess_PreTransformVertices |
		aiProcess_GenNormals |
		aiProcess_GenUVCoords |
		aiProcess_Debone |
		aiProcess_RemoveRedundantMaterials |
		aiProcess_FindDegenerates |
		aiProcess_GenBoundingBoxes |
		aiProcess_ValidateDataStructure;
}

//////////////////////////
// Assimp Helper Functions
TVector<std::string> TraceUsedTextures_Assimp(aiMaterial* mat, aiTextureType type)
{
	TVector<std::string> textures;
	for (uint32_t i = 0; i < mat->GetTextureCount(type); i++)
	{
		aiString str;
		mat->GetTexture(type, i, &str);
		textures.Add(str.C_Str());
	}
	return textures;
}

MaterialAsset::Data ProcessMaterial_Assimp(aiMesh* mesh, const aiScene* scene, const std::string& texturesFolder)
{
	MaterialAsset::Data data;

	aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];

	data.m_name = material->GetName().C_Str();

	const TVector<std::string> diffuseMaps = TraceUsedTextures_Assimp(material, aiTextureType_DIFFUSE);
	const TVector<std::string> specularMaps = TraceUsedTextures_Assimp(material, aiTextureType_SPECULAR);
	const TVector<std::string> ambientMaps = TraceUsedTextures_Assimp(material, aiTextureType_AMBIENT);
	const TVector<std::string> normalMaps = TraceUsedTextures_Assimp(material, aiTextureType_NORMALS);
	const TVector<std::string> emissionMaps = TraceUsedTextures_Assimp(material, aiTextureType_EMISSIVE);

	data.m_shader = App::GetSubmodule<AssetRegistry>()->GetOrLoadAsset("Shaders/Standard.shader");

	glm::vec4 diffuse{};
	glm::vec4 ambient{};
	glm::vec4 emission{};
	glm::vec4 specular{};

	auto FillData = [](const aiMaterialProperty* property, const std::string& name, glm::vec4* ptr)
		{
			const string key = property->mKey.C_Str();
			TVector<size_t> locations;
			Utils::FindAllOccurances(key, name, locations, 0);
			if (locations.Num() > 0)
			{
				memcpy(ptr, property->mData, property->mDataLength);
				return true;
			}

			return false;
		};

	for (uint32_t i = 0; i < material->mNumProperties; i++)
	{
		const auto property = material->mProperties[i];
		if (property->mType == aiPTI_Float && property->mDataLength >= sizeof(float) * 3)
		{
			if (FillData(property, std::string("diffuse"), &diffuse) ||
				FillData(property, std::string("ambient"), &ambient) ||
				FillData(property, std::string("emission"), &emission) ||
				FillData(property, std::string("specular"), &specular))
			{
				continue;
			}
		}
	}

	data.m_uniformsVec4.Add({ "material.albedo", diffuse });
	data.m_uniformsVec4.Add({ "material.ambient", ambient });
	data.m_uniformsVec4.Add({ "material.emission", emission });
	data.m_uniformsVec4.Add({ "material.specular", specular });

	data.m_uniformsFloat.Add({ "material.roughness" , 1.0f });
	data.m_uniformsFloat.Add({ "material.metallic" , 1.0f });

	// TODO: Add support for glb?

	if (!diffuseMaps.IsEmpty() && diffuseMaps[0].front() != '*')
	{
		data.m_samplers.Add(MaterialAsset::SamplerEntry("albedoSampler", App::GetSubmodule<AssetRegistry>()->GetOrLoadAsset(texturesFolder + diffuseMaps[0])));
	}

	if (!ambientMaps.IsEmpty())
	{
		data.m_samplers.Add(MaterialAsset::SamplerEntry("ambientSampler", App::GetSubmodule<AssetRegistry>()->GetOrLoadAsset(texturesFolder + ambientMaps[0])));
	}

	if (!normalMaps.IsEmpty() && normalMaps[0].front() != '*')
	{
		data.m_samplers.Add(MaterialAsset::SamplerEntry("normalSampler", App::GetSubmodule<AssetRegistry>()->GetOrLoadAsset(texturesFolder + normalMaps[0])));
	}

	if (!specularMaps.IsEmpty() && specularMaps[0].front() != '*')
	{
		data.m_samplers.Add(MaterialAsset::SamplerEntry("specularSampler", App::GetSubmodule<AssetRegistry>()->GetOrLoadAsset(texturesFolder + specularMaps[0])));
	}

	if (!emissionMaps.IsEmpty() && emissionMaps[0].front() != '*')
	{
		data.m_samplers.Add(MaterialAsset::SamplerEntry("emissionSampler", App::GetSubmodule<AssetRegistry>()->GetOrLoadAsset(texturesFolder + emissionMaps[0])));
	}

	return data;
}

void ProcessNodeMaterials_Assimp(TVector<MaterialAsset::Data>& outMaterials, aiNode* node, const aiScene* scene, const std::string& texturesFolder)
{
	for (uint32_t i = 0; i < node->mNumMeshes; i++)
	{
		aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
		outMaterials.Add(std::move(ProcessMaterial_Assimp(mesh, scene, texturesFolder)));
	}

	for (uint32_t i = 0; i < node->mNumChildren; i++)
	{
		ProcessNodeMaterials_Assimp(outMaterials, node->mChildren[i], scene, texturesFolder);
	}
}

ModelImporter::MeshContext ProcessMesh_Assimp(aiMesh* mesh, const aiScene* scene)
{
	assert(mesh->HasPositions());
	assert(mesh->HasNormals());

	Sailor::ModelImporter::MeshContext meshContext;
	meshContext.bounds.m_min = *(vec3*)(&mesh->mAABB.mMin);
	meshContext.bounds.m_max = *(vec3*)(&mesh->mAABB.mMax);

	for (uint32_t i = 0; i < mesh->mNumVertices; i++)
	{
		RHI::VertexP3N3T3B3UV2C4 vertex{};

		vertex.m_position =
		{
			mesh->mVertices[i].x,
			mesh->mVertices[i].y,
			mesh->mVertices[i].z
		};

		if (mesh->HasTextureCoords(0))
		{
			vertex.m_texcoord =
			{
				mesh->mTextureCoords[0][i].x,
				mesh->mTextureCoords[0][i].y
			};
		}

		vertex.m_color = { 1.0f, 1.0f, 1.0f, 1.0f };

		vertex.m_normal =
		{
			mesh->mNormals[i].x,
			mesh->mNormals[i].y,
			mesh->mNormals[i].z
		};

		if (mesh->HasTangentsAndBitangents())
		{
			vertex.m_tangent =
			{
				mesh->mTangents[i].x,
				mesh->mTangents[i].y,
				mesh->mTangents[i].z
			};

			vertex.m_bitangent =
			{
				mesh->mBitangents[i].x,
				mesh->mBitangents[i].y,
				mesh->mBitangents[i].z
			};
		}

		meshContext.outVertices.Add(std::move(vertex));
	}

	for (uint32_t i = 0; i < mesh->mNumFaces; i++)
	{
		aiFace face = mesh->mFaces[i];
		for (uint32_t j = 0; j < face.mNumIndices; j++)
		{
			meshContext.outIndices.Add(face.mIndices[j]);
		}
	}

	return meshContext;
}

void ProcessNode_Assimp(TVector<ModelImporter::MeshContext>& outParsedMeshes, aiNode* node, const aiScene* scene)
{
	for (uint32_t i = 0; i < node->mNumMeshes; i++)
	{
		aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
		outParsedMeshes.Add(std::move(ProcessMesh_Assimp(mesh, scene)));
	}

	for (uint32_t i = 0; i < node->mNumChildren; i++)
	{
		ProcessNode_Assimp(outParsedMeshes, node->mChildren[i], scene);
	}
}
//////////////////////////

void Model::Flush()
{
	if (m_meshes.Num() == 0)
	{
		m_bIsReady = false;
		return;
	}

	for (const auto& mesh : m_meshes)
	{
		if (!mesh)// || !mesh->IsReady())
		{
			m_bIsReady = false;
			return;
		}
	}

	m_bIsReady = true;
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
	if (ModelAssetInfoPtr modelAssetInfo = dynamic_cast<ModelAssetInfoPtr>(assetInfo))
	{
		if (modelAssetInfo->ShouldGenerateMaterials() && modelAssetInfo->GetDefaultMaterials().Num() == 0)
		{
			GenerateMaterialAssets(modelAssetInfo);
			assetInfo->SaveMetaFile();
		}
	}
}

void ModelImporter::OnImportAsset(AssetInfoPtr assetInfo)
{
}

void ModelImporter::GenerateMaterialAssets(ModelAssetInfoPtr assetInfo)
{
	SAILOR_PROFILE_FUNCTION();

	Assimp::Importer importer;
	const auto ImportFlags = DefaultImportFlags_Assimp | (assetInfo->ShouldBatchByMaterial() ? aiProcess_OptimizeMeshes : 0);
	const auto scene = importer.ReadFile(assetInfo->GetAssetFilepath().c_str(), ImportFlags);

	if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
	{
		SAILOR_LOG("%s", importer.GetErrorString());
		return;
	}

	const std::string texturesFolder = Utils::GetFileFolder(assetInfo->GetRelativeAssetFilepath());

	TVector<MaterialAsset::Data> materials;
	ProcessNodeMaterials_Assimp(materials, scene->mRootNode, scene, texturesFolder);

	for (const auto& material : materials)
	{
		std::string materialsFolder = AssetRegistry::ContentRootFolder + texturesFolder + "materials/";
		std::filesystem::create_directory(materialsFolder);

		FileId materialFileId = App::GetSubmodule<MaterialImporter>()->CreateMaterialAsset(materialsFolder + material.m_name + ".mat", std::move(material));
		assetInfo->GetDefaultMaterials().Add(materialFileId);
	}
}

bool ModelImporter::LoadModel_Immediate(FileId uid, ModelPtr& outModel)
{
	SAILOR_PROFILE_FUNCTION();

	auto task = LoadModel(uid, outModel);
	task->Wait();
	return task->GetResult().IsValid();
}

Tasks::TaskPtr<ModelPtr> ModelImporter::LoadModel(FileId uid, ModelPtr& outModel)
{
	SAILOR_PROFILE_FUNCTION();

	// Check promises first
	auto& promise = m_promises.At_Lock(uid, nullptr);
	auto& loadedModel = m_loadedModels.At_Lock(uid, ModelPtr());

	// Check loaded assets
	if (loadedModel)
	{
		outModel = loadedModel;
		auto res = promise ? promise : Tasks::TaskPtr<ModelPtr>::Make(outModel);

		m_loadedModels.Unlock(uid);
		m_promises.Unlock(uid);

		return res;
	}

	// There is no promise, we need to load model
	if (ModelAssetInfoPtr assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<ModelAssetInfoPtr>(uid))
	{
		ModelPtr model = ModelPtr::Make(m_allocator, uid);

		// The way to drop qualifiers inside lambda
		auto& boundsSphere = model->m_boundsSphere;
		auto& boundsAabb = model->m_boundsAabb;

		struct Data
		{
			TVector<MeshContext> m_parsedMeshes;
			bool m_bIsImported = false;
		};

		promise = Tasks::CreateTaskWithResult<TSharedPtr<Data>>("Load model",
			[model, assetInfo, this, &boundsAabb, &boundsSphere]()
			{
				TSharedPtr<Data> res = TSharedPtr<Data>::Make();
				res->m_bIsImported = ImportModel(assetInfo, res->m_parsedMeshes, boundsAabb, boundsSphere);
				return res;
			})->Then<ModelPtr>([model](TSharedPtr<Data> data) mutable
				{
					if (data->m_bIsImported)
					{
						for (const auto& mesh : data->m_parsedMeshes)
						{
							RHI::RHIMeshPtr ptr = RHI::Renderer::GetDriver()->CreateMesh();
							ptr->m_vertexDescription = RHI::Renderer::GetDriver()->GetOrAddVertexDescription<RHI::VertexP3N3T3B3UV2C4>();
							ptr->m_bounds = mesh.bounds;
							RHI::Renderer::GetDriver()->UpdateMesh(ptr,
								&mesh.outVertices[0], sizeof(RHI::VertexP3N3T3B3UV2C4) * mesh.outVertices.Num(),
								&mesh.outIndices[0], sizeof(uint32_t) * mesh.outIndices.Num());

							model->m_meshes.Emplace(ptr);
						}

						model->Flush();
					}
					return model;
				}, "Update RHI Meshes", Tasks::EThreadType::RHI)->ToTaskWithResult();

				outModel = loadedModel = model;
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

bool ModelImporter::ImportModel(ModelAssetInfoPtr assetInfo, TVector<MeshContext>& outParsedMeshes, Math::AABB& outBoundsAabb, Math::Sphere& outBoundsSphere)
{
	Assimp::Importer importer;

	outBoundsAabb.m_max = glm::vec3(std::numeric_limits<float>::min());
	outBoundsAabb.m_min = glm::vec3(std::numeric_limits<float>::max());

	const auto ImportFlags = DefaultImportFlags_Assimp | (assetInfo->ShouldBatchByMaterial() ? (aiProcess_OptimizeMeshes | aiProcess_ImproveCacheLocality) : 0);
	const auto scene = importer.ReadFile(assetInfo->GetAssetFilepath().c_str(), ImportFlags);

	if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
	{
		SAILOR_LOG("%s", importer.GetErrorString());
		return false;
	}

	ProcessNode_Assimp(outParsedMeshes, scene->mRootNode, scene);

	for (const auto& mesh : outParsedMeshes)
	{
		outBoundsAabb.Extend(mesh.bounds);
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

void ModelImporter::CollectGarbage()
{
	TVector<FileId> uidsToRemove;
	for (auto& promise : m_promises)
	{
		if (promise.m_second->IsFinished())
		{
			FileId uid = promise.m_first;
			uidsToRemove.Emplace(uid);
		}
	}

	for (auto& uid : uidsToRemove)
	{
		m_promises.Remove(uid);
	}
}