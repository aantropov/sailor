#include "ModelImporter.h"
#include "AssetRegistry/UID.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "ModelAssetInfo.h"
#include "Core/Utils.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>
#include "RHI/Types.h"
#include "Memory/ObjectAllocator.hpp"

#include "nlohmann_json/include/nlohmann/json.hpp"
#include "JobSystem/JobSystem.h"

#define TINYOBJLOADER_IMPLEMENTATION 
#include "tiny_obj_loader/tiny_obj_loader.h"

using namespace Sailor;

void Model::Flush()
{
	if (m_meshes.Num() == 0)
	{
		m_bIsReady = false;
		return;
	}

	/*
		for (const auto& mesh : m_meshes)
		{
			if (!mesh || !mesh->IsReady())
			{
				m_bIsReady = false;
				return;
			}
		}

		for (const auto& material : m_materials)
		{
			if (!material || !material.Lock()->IsReady())
			{
				m_bIsReady = false;
			}
		}
	*/
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

		data.m_shader = App::GetSubmodule<AssetRegistry>()->GetOrLoadAsset("Shaders/Simple.shader");

		glm::vec4 diffuse, ambient, emission, specular;
		memcpy(&diffuse, material.diffuse, 3 * sizeof(tinyobj::real_t));
		memcpy(&ambient, material.ambient, 3 * sizeof(tinyobj::real_t));
		memcpy(&emission, material.emission, 3 * sizeof(tinyobj::real_t));
		memcpy(&specular, material.specular, 3 * sizeof(tinyobj::real_t));
		data.m_uniformsVec4.Add({ "material.diffuse", diffuse });
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

		if (!material.normal_texname.empty())
		{
			data.m_samplers.Add(MaterialAsset::SamplerEntry("normalSampler", App::GetSubmodule<AssetRegistry>()->GetOrLoadAsset(texturesFolder + material.normal_texname)));
		}

		std::string materialsFolder = AssetRegistry::ContentRootFolder + texturesFolder + "materials/";
		std::filesystem::create_directory(materialsFolder);

		UID materialUID = App::GetSubmodule<MaterialImporter>()->CreateMaterialAsset(materialsFolder + material.name + ".mat", std::move(data));
		assetInfo->GetDefaultMaterials().Add(materialUID);
	}

	assetInfo->SaveMetaFile();
}

bool ModelImporter::LoadModel_Immediate(UID uid, ModelPtr& outModel)
{
	SAILOR_PROFILE_FUNCTION();

	auto task = LoadModel(uid, outModel);
	task->Wait();
	return task->GetResult();
}

JobSystem::TaskPtr<bool> ModelImporter::LoadModel(UID uid, ModelPtr& outModel)
{
	SAILOR_PROFILE_FUNCTION();

	JobSystem::TaskPtr<bool> newPromise;
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
				return JobSystem::TaskPtr<bool>::Make(true);
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

		newPromise = JobSystem::Scheduler::CreateTaskWithResult<bool>("Load model",
			[model, assetInfo, this]()
		{
			bool bRes = ImportObjModel(assetInfo, model.GetRawPtr()->m_meshes);
			model.GetRawPtr()->Flush();
			return bRes;
		});

		App::GetSubmodule<JobSystem::Scheduler>()->Run(newPromise);

		outModel = m_loadedModels[uid] = model;
		promise = newPromise;
		m_promises.Unlock(uid);

		return promise;
	}

	m_promises.Unlock(uid);
	return JobSystem::TaskPtr<bool>::Make(false);
}

bool ModelImporter::ImportObjModel(ModelAssetInfoPtr assetInfo, TVector<RHI::MeshPtr>& outMeshes)
{
	tinyobj::attrib_t attrib;
	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> materials;
	std::string warn;
	std::string err;

	auto assetFilepath = assetInfo->GetAssetFilepath();
	auto materialFolder = Utils::GetFileFolder(assetFilepath);
	const std::string materialsFolder = Utils::GetFileFolder(assetInfo->GetRelativeAssetFilepath()) + "materials/";
	const bool bHasMaterialUIDsInMeta = assetInfo->GetDefaultMaterials().Num() > 0;

	if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, assetInfo->GetAssetFilepath().c_str(), materialFolder.c_str()))
	{
		SAILOR_LOG("%s %s", warn.c_str(), err.c_str());
		return false;
	}

	struct MeshContext
	{
		std::unordered_map<RHI::Vertex, uint32_t> uniqueVertices;
		TVector<RHI::Vertex> outVertices;
		TVector<uint32_t> outIndices;
	};

	TVector<MeshContext> meshes;
	meshes.Resize(assetInfo->ShouldBatchByMaterial() ? materials.size() : shapes.size());

	uint32_t idx = 0;
	for (const auto& shape : shapes)
	{
		auto& mesh = meshes[assetInfo->ShouldBatchByMaterial() ? shape.mesh.material_ids[0] : idx];
		for (const auto& index : shape.mesh.indices)
		{
			RHI::Vertex vertex{};

			vertex.m_position =
			{
			attrib.vertices[3 * index.vertex_index + 0],
			attrib.vertices[3 * index.vertex_index + 1],
			attrib.vertices[3 * index.vertex_index + 2]
			};

			vertex.m_texcoord =
			{
				attrib.texcoords[2 * index.texcoord_index + 0],
				1.0f - attrib.texcoords[2 * index.texcoord_index + 1]
			};

			vertex.m_color = { 1.0f, 1.0f, 1.0f, 1.0f };

			vertex.m_normal =
			{
				attrib.normals[3 * index.normal_index + 0],
				attrib.normals[3 * index.normal_index + 1],
				attrib.normals[3 * index.normal_index + 2]
			};

			if (mesh.uniqueVertices.count(vertex) == 0)
			{
				mesh.uniqueVertices[vertex] = static_cast<uint32_t>(mesh.outVertices.Num());
				mesh.outVertices.Add(vertex);
			}

			mesh.outIndices.Add(mesh.uniqueVertices[vertex]);
		}

		idx++;
	}

	for (const auto& mesh : meshes)
	{
		RHI::MeshPtr ptr = RHI::Renderer::GetDriver()->CreateMesh();
		RHI::Renderer::GetDriver()->UpdateMesh(ptr, mesh.outVertices, mesh.outIndices);
		outMeshes.Emplace(ptr);
	}

	return true;
}

JobSystem::TaskPtr<bool> ModelImporter::LoadDefaultMaterials(UID uid, TVector<MaterialPtr>& outMaterials)
{
	outMaterials.Clear();

	if (ModelAssetInfoPtr modelInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<ModelAssetInfoPtr>(uid))
	{
		JobSystem::TaskPtr<bool> loadingFinished = JobSystem::Scheduler::CreateTaskWithResult<bool>("Load Default Materials", []() { return true; });

		for (auto& assetInfo : modelInfo->GetDefaultMaterials())
		{
			MaterialPtr material;
			JobSystem::ITaskPtr loadMaterial;
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

		App::GetSubmodule<JobSystem::Scheduler>()->Run(loadingFinished);
		return loadingFinished;
	}

	return JobSystem::TaskPtr<bool>::Make(false);
}
