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
#include "Memory/Heap.h"

#include "nlohmann_json/include/nlohmann/json.hpp"
#include "JobSystem/JobSystem.h"

#define TINYOBJLOADER_IMPLEMENTATION 
#include "tiny_obj_loader/tiny_obj_loader.h"

using namespace Sailor;

void Model::Flush()
{
	if (m_meshes.size() == 0 || m_meshes.size() != m_materials.size())
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
	infoHandler->Subscribe(this);
}

ModelImporter::~ModelImporter()
{
	m_loadedModels.clear();
}

void ModelImporter::OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired)
{
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
	for (const auto& material : materials)
	{
		MaterialAsset::Data data;

		data.m_shader = App::GetSubmodule<AssetRegistry>()->GetOrLoadAsset("Shaders/Simple.shader");

		glm::vec4 diffuse, ambient, emission, specular;
		memcpy(&diffuse, material.diffuse, 3 * sizeof(tinyobj::real_t));
		memcpy(&ambient, material.ambient, 3 * sizeof(tinyobj::real_t));
		memcpy(&emission, material.emission, 3 * sizeof(tinyobj::real_t));
		memcpy(&specular, material.specular, 3 * sizeof(tinyobj::real_t));
		data.m_uniformsVec4.push_back({ "material.diffuse", diffuse });
		data.m_uniformsVec4.push_back({ "material.ambient", ambient });
		data.m_uniformsVec4.push_back({ "material.emission", emission });
		data.m_uniformsVec4.push_back({ "material.specular", specular });

		if (!material.diffuse_texname.empty())
		{
			data.m_samplers.push_back(MaterialAsset::SamplerEntry("diffuseSampler", App::GetSubmodule<AssetRegistry>()->GetOrLoadAsset(texturesFolder + material.diffuse_texname)));
		}

		if (!material.ambient_texname.empty())
		{
			data.m_samplers.push_back(MaterialAsset::SamplerEntry("ambientSampler", App::GetSubmodule<AssetRegistry>()->GetOrLoadAsset(texturesFolder + material.ambient_texname)));
		}

		if (!material.normal_texname.empty())
		{
			data.m_samplers.push_back(MaterialAsset::SamplerEntry("normalSampler", App::GetSubmodule<AssetRegistry>()->GetOrLoadAsset(texturesFolder + material.normal_texname)));
		}

		std::string materialsFolder = AssetRegistry::ContentRootFolder + texturesFolder + "materials/";
		std::filesystem::create_directory(materialsFolder);

		App::GetSubmodule<MaterialImporter>()->CreateMaterialAsset(materialsFolder + material.name + ".mat", std::move(data));
	}
}

bool ModelImporter::LoadModel_Immediate(UID uid, ModelPtr& outModel)
{
	SAILOR_PROFILE_FUNCTION();

	auto it = m_loadedModels.find(uid);
	if (it != m_loadedModels.end())
	{
		return outModel = (*it).second;
	}

	if (ModelAssetInfoPtr assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<ModelAssetInfoPtr>(uid))
	{
		auto model = TSharedPtr<Model>::Make(uid);
		std::vector<AssetInfoPtr> outMaterialUIDs;

		if (ImportObjModel(assetInfo, model->m_meshes, outMaterialUIDs))
		{
			for (auto& assetInfo : outMaterialUIDs)
			{
				MaterialPtr material;
				if (assetInfo && App::GetSubmodule<MaterialImporter>()->LoadMaterial_Immediate(assetInfo->GetUID(), material))
				{
					if (material)
					{
						model.GetRawPtr()->m_materials.push_back(material);
						model.GetRawPtr()->AddHotReloadDependentObject(material);
					}
					else
					{
						//TODO: push missing material
					}
				}
			}
			model->Flush();

			{
				std::scoped_lock<std::mutex> guard(m_mutex);
				return outModel = m_loadedModels[uid] = model;
			}
		}
	}

	return false;
}

JobSystem::TaskPtr<bool> ModelImporter::LoadModel(UID uid, ModelPtr& outModel)
{
	SAILOR_PROFILE_FUNCTION();

	auto it = m_loadedModels.find(uid);
	if (it != m_loadedModels.end())
	{
		outModel = (*it).second;
		return JobSystem::TaskPtr<bool>::Make(true);
	}

	JobSystem::TaskPtr<bool> outLoadingTask;

	if (ModelAssetInfoPtr assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<ModelAssetInfoPtr>(uid))
	{
		auto model = TSharedPtr<Model>::Make(uid);

		outLoadingTask = JobSystem::Scheduler::CreateTaskWithResult<bool>("Load model",
			[model, assetInfo, this]()
			{
				std::vector<AssetInfoPtr> outMaterialUIDs;
				if (ImportObjModel(assetInfo, model.GetRawPtr()->m_meshes, outMaterialUIDs))
				{
					JobSystem::ITaskPtr flushModel = JobSystem::Scheduler::CreateTask("Flush model",
						[model]()
						{
							model.GetRawPtr()->Flush();
						});

					for (auto& assetInfo : outMaterialUIDs)
					{
						MaterialPtr material;
						JobSystem::ITaskPtr loadMaterial;
						if (assetInfo && (loadMaterial = App::GetSubmodule<MaterialImporter>()->LoadMaterial(assetInfo->GetUID(), material)))
						{
							if (material)
							{
								model.GetRawPtr()->m_materials.push_back(material);
								model.GetRawPtr()->AddHotReloadDependentObject(material);

								if (loadMaterial)
								{
									flushModel->Join(loadMaterial);
								}
							}
							else
							{
								//TODO: push missing material
							}
						}
					}

					App::GetSubmodule<JobSystem::Scheduler>()->Run(flushModel);

					return true;
				}
				return false;
			});

		App::GetSubmodule<JobSystem::Scheduler>()->Run(outLoadingTask);

		{
			std::scoped_lock<std::mutex> guard(m_mutex);
			outModel = m_loadedModels[uid] = model;
		}

		return outLoadingTask;
	}

	return JobSystem::TaskPtr<bool>::Make(false);
}

bool ModelImporter::ImportObjModel(ModelAssetInfoPtr assetInfo,
	std::vector<RHI::MeshPtr>& outMeshes,
	std::vector<AssetInfoPtr>& outMaterialUIDs)
{

	tinyobj::attrib_t attrib;
	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> materials;
	std::string warn;
	std::string err;

	auto assetFilepath = assetInfo->GetAssetFilepath();
	auto materialFolder = Utils::GetFileFolder(assetFilepath);
	const std::string materialsFolder = Utils::GetFileFolder(assetInfo->GetRelativeAssetFilepath()) + "materials/";

	if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, assetInfo->GetAssetFilepath().c_str(), materialFolder.c_str()))
	{
		SAILOR_LOG("%s %s", warn.c_str(), err.c_str());
		return false;
	}

	struct MeshContext
	{
		std::unordered_map<RHI::Vertex, uint32_t> uniqueVertices;
		std::vector<RHI::Vertex> outVertices;
		std::vector<uint32_t> outIndices;
	};

	std::vector<MeshContext> meshes;
	meshes.resize(assetInfo->ShouldBatchByMaterial() ? materials.size() : shapes.size());

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

			if (mesh.uniqueVertices.count(vertex) == 0)
			{
				mesh.uniqueVertices[vertex] = static_cast<uint32_t>(mesh.outVertices.size());
				mesh.outVertices.push_back(vertex);
			}

			mesh.outIndices.push_back(mesh.uniqueVertices[vertex]);
		}

		if (assetInfo->ShouldGenerateMaterials() && !assetInfo->ShouldBatchByMaterial())
		{
			if (AssetInfoPtr materialInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<AssetInfoPtr>(materialsFolder + materials[shape.mesh.material_ids[0]].name + ".mat"))
			{
				outMaterialUIDs.push_back(materialInfo);
			}
			else
			{
				outMaterialUIDs.push_back(nullptr);
			}
		}

		idx++;
	}

	for (const auto& mesh : meshes)
	{
		RHI::MeshPtr ptr = RHI::Renderer::GetDriver()->CreateMesh();
		RHI::Renderer::GetDriver()->UpdateMesh(ptr, mesh.outVertices, mesh.outIndices);
		outMeshes.emplace_back(ptr);
	}

	if (assetInfo->ShouldGenerateMaterials() && assetInfo->ShouldBatchByMaterial())
	{
		for (const auto& material : materials)
		{
			if (AssetInfoPtr materialInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<AssetInfoPtr>(materialsFolder + material.name + ".mat"))
			{
				outMaterialUIDs.push_back(materialInfo);
			}
			else
			{
				outMaterialUIDs.push_back(nullptr);
			}
		}
	}

	return true;
}