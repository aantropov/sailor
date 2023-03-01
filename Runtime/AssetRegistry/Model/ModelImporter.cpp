#include "ModelImporter.h"
#include "AssetRegistry/UID.h"
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

#define TINYOBJLOADER_IMPLEMENTATION 
#include "tiny_obj_loader/tiny_obj_loader.h"

using namespace Sailor;

namespace {
	const unsigned int ImportFlags =
		aiProcess_CalcTangentSpace |
		aiProcess_Triangulate |
		aiProcess_FlipUVs |
		aiProcess_SortByPType |
		aiProcess_PreTransformVertices |
		aiProcess_GenNormals |
		aiProcess_GenUVCoords |
		aiProcess_OptimizeMeshes |
		aiProcess_Debone |
		aiProcess_ValidateDataStructure;
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
	m_allocator = ObjectAllocatorPtr::Make();
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
	if (ModelAssetInfoPtr modelAssetInfo = dynamic_cast<ModelAssetInfoPtr>(assetInfo))
	{
		if (modelAssetInfo->ShouldGenerateMaterials())
		{
			GenerateMaterialAssets(modelAssetInfo);
		}
	}
}

void ModelImporter::GenerateMaterialAssets(ModelAssetInfoPtr assetInfo)
{
	SAILOR_PROFILE_FUNCTION();

	tinyobj::attrib_t attrib;
	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> materials;
	std::string warn;
	std::string err;

	auto assetFilepath = assetInfo->GetAssetFilepath();
	auto materialFolder = Utils::GetFileFolder(assetFilepath);
	if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, assetInfo->GetAssetFilepath().c_str(), materialFolder.c_str()))
	{
		SAILOR_LOG("%s %s", warn.c_str(), err.c_str());
		return;
	}

	auto texturesFolder = Utils::GetFileFolder(assetInfo->GetRelativeAssetFilepath());
	std::vector<tinyobj::material_t> materialsOrder;

	if (!assetInfo->ShouldBatchByMaterial())
	{
		for (const auto& shape : shapes)
		{
			materialsOrder.push_back(materials[shape.mesh.material_ids[0]]);
		}
	}
	else
	{
		materialsOrder = materials;
	}

	for (const auto& material : materialsOrder)
	{
		MaterialAsset::Data data;

		data.m_shader = App::GetSubmodule<AssetRegistry>()->GetOrLoadAsset("Shaders/Standard.shader");

		glm::vec4 diffuse{};
		glm::vec4 ambient{};
		glm::vec4 emission{};
		glm::vec4 specular{};

		memcpy(&diffuse, material.diffuse, 3 * sizeof(tinyobj::real_t));
		memcpy(&ambient, material.ambient, 3 * sizeof(tinyobj::real_t));
		memcpy(&emission, material.emission, 3 * sizeof(tinyobj::real_t));
		memcpy(&specular, material.specular, 3 * sizeof(tinyobj::real_t));
		data.m_uniformsVec4.Add({ "material.albedo", diffuse });
		data.m_uniformsVec4.Add({ "material.ambient", ambient });
		data.m_uniformsVec4.Add({ "material.emission", emission });
		data.m_uniformsVec4.Add({ "material.specular", specular });

		if (!material.diffuse_texname.empty())
		{
			data.m_samplers.Add(MaterialAsset::SamplerEntry("diffuseSampler", App::GetSubmodule<AssetRegistry>()->GetOrLoadAsset(texturesFolder + material.diffuse_texname)));
		}

		if (!material.ambient_texname.empty())
		{
			data.m_samplers.Add(MaterialAsset::SamplerEntry("ambientSampler", App::GetSubmodule<AssetRegistry>()->GetOrLoadAsset(texturesFolder + material.ambient_texname)));
		}

		if (!material.bump_texname.empty())
		{
			data.m_samplers.Add(MaterialAsset::SamplerEntry("normalSampler", App::GetSubmodule<AssetRegistry>()->GetOrLoadAsset(texturesFolder + material.bump_texname)));
		}

		if (!material.normal_texname.empty())
		{
			data.m_samplers.Add(MaterialAsset::SamplerEntry("normalSampler", App::GetSubmodule<AssetRegistry>()->GetOrLoadAsset(texturesFolder + material.normal_texname)));
		}

		if (!material.specular_texname.empty())
		{
			data.m_samplers.Add(MaterialAsset::SamplerEntry("specularSampler", App::GetSubmodule<AssetRegistry>()->GetOrLoadAsset(texturesFolder + material.specular_texname)));
		}

		if (!material.emissive_texname.empty())
		{
			data.m_samplers.Add(MaterialAsset::SamplerEntry("emissionSampler", App::GetSubmodule<AssetRegistry>()->GetOrLoadAsset(texturesFolder + material.emissive_texname)));
		}

		std::string materialsFolder = AssetRegistry::ContentRootFolder + texturesFolder + "materials/";
		std::filesystem::create_directory(materialsFolder);

		UID materialUID = App::GetSubmodule<MaterialImporter>()->CreateMaterialAsset(materialsFolder + material.name + ".mat", std::move(data));
		assetInfo->GetDefaultMaterials().Add(materialUID);
	}
}

bool ModelImporter::LoadModel_Immediate(UID uid, ModelPtr& outModel)
{
	SAILOR_PROFILE_FUNCTION();

	auto task = LoadModel(uid, outModel);
	task->Wait();
	return task->GetResult().IsValid();
}

Tasks::TaskPtr<ModelPtr> ModelImporter::LoadModel(UID uid, ModelPtr& outModel)
{
	SAILOR_PROFILE_FUNCTION();

	Tasks::TaskPtr<ModelPtr> newPromise;
	outModel = nullptr;

	// Check promises first
	auto it = m_promises.Find(uid);
	if (it != m_promises.end())
	{
		newPromise = (*it).m_second;
	}

	// Check loaded model
	auto modelIt = m_loadedModels.Find(uid);
	if (modelIt != m_loadedModels.end())
	{
		outModel = (*modelIt).m_second;

		if (newPromise != nullptr)
		{
			if (!newPromise)
			{
				return Tasks::TaskPtr<ModelPtr>::Make(outModel);
			}

			return newPromise;
		}
	}

	auto& promise = m_promises.At_Lock(uid, nullptr);

	// We have promise
	if (promise)
	{
		m_promises.Unlock(uid);
		outModel = m_loadedModels[uid];
		return promise;
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

		newPromise = Tasks::Scheduler::CreateTaskWithResult<TSharedPtr<Data>>("Load model",
			[model, assetInfo, this, &boundsAabb, &boundsSphere]()
			{
				TSharedPtr<Data> res = TSharedPtr<Data>::Make();
				res->m_bIsImported = ImportModel(assetInfo, res->m_parsedMeshes, boundsAabb, boundsSphere);
				return res;
			})->Then<ModelPtr, TSharedPtr<Data>>([model](TSharedPtr<Data> data) mutable
				{
					if (data->m_bIsImported)
					{
						for (const auto& mesh : data->m_parsedMeshes)
						{
							if (!mesh.bIsInited)
							{
								continue;
							}

							RHI::RHIMeshPtr ptr = RHI::Renderer::GetDriver()->CreateMesh();
							ptr->m_vertexDescription = RHI::Renderer::GetDriver()->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
							ptr->m_bounds = mesh.bounds;
							RHI::Renderer::GetDriver()->UpdateMesh(ptr,
								&mesh.outVertices[0], sizeof(RHI::VertexP3N3UV2C4) * mesh.outVertices.Num(),
								&mesh.outIndices[0], sizeof(uint32_t) * mesh.outIndices.Num());

							model->m_meshes.Emplace(ptr);
						}

						model->Flush();
					}
					return model;
				}, "Update RHI Meshes", Tasks::EThreadType::RHI)->ToTaskWithResult();

				outModel = m_loadedModels[uid] = model;
				newPromise->Run();
				promise = newPromise;
				m_promises.Unlock(uid);

				return promise;
	}

	m_promises.Unlock(uid);
	return Tasks::TaskPtr<ModelPtr>();
}

ModelImporter::MeshContext ProcessAssimpMesh(aiMesh* mesh, const aiScene* scene)
{
	Sailor::ModelImporter::MeshContext meshContext;

	for (uint32_t i = 0; i < mesh->mNumVertices; i++)
	{
		RHI::VertexP3N3UV2C4 vertex{};

		vertex.m_position =
		{
			mesh->mVertices[i].x,
			mesh->mVertices[i].y,
			mesh->mVertices[i].z
		};

		if(mesh->HasTextureCoords(0))
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

		meshContext.outVertices.Add(std::move(vertex));

		if (!meshContext.bIsInited)
		{
			meshContext.bounds.m_max = meshContext.bounds.m_min = vertex.m_position;
			meshContext.bIsInited = true;
		}

		meshContext.bounds.Extend(vertex.m_position);
	}
	
	for (uint32_t i = 0; i < mesh->mNumFaces; i++)
	{
		aiFace face = mesh->mFaces[i];
		for (uint32_t j = 0; j < face.mNumIndices; j++)
		{
			meshContext.outIndices.Add(face.mIndices[j]);
		}
	}

	if (mesh->mMaterialIndex >= 0)
	{
	}

	return meshContext;
}

void ProcessAssimpNode(TVector<ModelImporter::MeshContext>& outParsedMeshes, aiNode* node, const aiScene* scene)
{
	for (uint32_t i = 0; i < node->mNumMeshes; i++)
	{
		aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
		outParsedMeshes.Add(std::move(ProcessAssimpMesh(mesh, scene)));
	}

	for (uint32_t i = 0; i < node->mNumChildren; i++)
	{
		ProcessAssimpNode(outParsedMeshes, node->mChildren[i], scene);
	}
}

bool ModelImporter::ImportModel(ModelAssetInfoPtr assetInfo, TVector<MeshContext>& outParsedMeshes, Math::AABB& outBoundsAabb, Math::Sphere& outBoundsSphere)
{
	Assimp::Importer importer;

	const auto scene = importer.ReadFile(assetInfo->GetAssetFilepath().c_str(), ImportFlags);

	if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
	{
		SAILOR_LOG("%s", importer.GetErrorString());
		return false;
	}

	ProcessAssimpNode(outParsedMeshes, scene->mRootNode, scene);

	for (const auto& mesh : outParsedMeshes)
	{
		outBoundsAabb.Extend(mesh.bounds);
	}

	outBoundsSphere.m_center = 0.5f * (outBoundsAabb.m_min + outBoundsAabb.m_max);
	outBoundsSphere.m_radius = glm::distance(outBoundsAabb.m_max, outBoundsSphere.m_center);

	return true;
}

Tasks::TaskPtr<bool> ModelImporter::LoadDefaultMaterials(UID uid, TVector<MaterialPtr>& outMaterials)
{
	outMaterials.Clear();

	if (ModelAssetInfoPtr modelInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<ModelAssetInfoPtr>(uid))
	{
		Tasks::TaskPtr<bool> loadingFinished = Tasks::Scheduler::CreateTaskWithResult<bool>("Load Default Materials", []() { return true; });

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
	TVector<UID> uidsToRemove;
	for (auto& promise : m_promises)
	{
		if (promise.m_second->IsFinished())
		{
			UID uid = promise.m_first;
			uidsToRemove.Emplace(uid);
		}
	}

	for (auto& uid : uidsToRemove)
	{
		m_promises.Remove(uid);
	}
}