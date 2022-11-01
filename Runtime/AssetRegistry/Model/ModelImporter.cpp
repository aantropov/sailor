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

#include "nlohmann_json/include/nlohmann/json.hpp"
#include "Tasks/Scheduler.h"

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
			bool m_bIsImported;
		};

		newPromise = Tasks::Scheduler::CreateTaskWithResult<TSharedPtr<Data>>("Load model",
			[model, assetInfo, this, &boundsAabb, &boundsSphere]()
		{
			TSharedPtr<Data> res = TSharedPtr<Data>::Make();
			res->m_bIsImported = ImportObjModel(assetInfo, res->m_parsedMeshes, boundsAabb, boundsSphere);
			return res;
		})->Then<ModelPtr, TSharedPtr<Data>>([model](TSharedPtr<Data> data)
		{
			if (data->m_bIsImported)
			{
				for (const auto& mesh : data->m_parsedMeshes)
				{
					RHI::RHIMeshPtr ptr = RHI::Renderer::GetDriver()->CreateMesh();
					ptr->m_vertexDescription = RHI::Renderer::GetDriver()->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
					ptr->m_bounds = mesh.bounds;
					RHI::Renderer::GetDriver()->UpdateMesh(ptr, mesh.outVertices, mesh.outIndices);
					model.GetRawPtr()->m_meshes.Emplace(ptr);
				}

				model.GetRawPtr()->Flush();
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

bool ModelImporter::ImportObjModel(ModelAssetInfoPtr assetInfo, TVector<MeshContext>& outParsedMeshes, Math::AABB& outBoundsAabb, Math::Sphere& outBoundsSphere)
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

	outParsedMeshes.Resize(assetInfo->ShouldBatchByMaterial() ? materials.size() : shapes.size());

	uint32_t idx = 0;
	for (const auto& shape : shapes)
	{
		auto& mesh = outParsedMeshes[assetInfo->ShouldBatchByMaterial() ? shape.mesh.material_ids[0] : idx];
		for (const auto& index : shape.mesh.indices)
		{
			RHI::VertexP3N3UV2C4 vertex{};

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

			// Calculate bounds 
			if (!mesh.bIsInited)
			{
				mesh.bounds.m_max = mesh.bounds.m_min = vertex.m_position;
				mesh.bIsInited = true;
			}

			mesh.bounds.Extend(vertex.m_position);
			outBoundsAabb.Extend(mesh.bounds);

			if (mesh.uniqueVertices.count(vertex) == 0)
			{
				mesh.uniqueVertices[vertex] = static_cast<uint32_t>(mesh.outVertices.Num());
				mesh.outVertices.Add(vertex);
			}

			mesh.outIndices.Add(mesh.uniqueVertices[vertex]);
		}

		idx++;
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