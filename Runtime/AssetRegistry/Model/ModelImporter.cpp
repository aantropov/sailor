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

ModelImporter::ModelImporter(ModelAssetInfoHandler* infoHandler)
{
	SAILOR_PROFILE_FUNCTION();
	infoHandler->Subscribe(this);
}

ModelImporter::~ModelImporter()
{
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

TSharedPtr<JobSystem::Job> ModelImporter::LoadModel(UID uid, std::vector<RHI::MeshPtr>& outMeshes, std::vector<RHI::MaterialPtr>& outMaterials)
{
	SAILOR_PROFILE_FUNCTION();

	if (ModelAssetInfoPtr assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<ModelAssetInfoPtr>(uid))
	{
		auto jobLoad = JobSystem::Scheduler::CreateJob("Check unique vertices",
			[&outMeshes, &outMaterials, assetInfo]()
			{
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
				}

				for (auto mesh : meshes)
				{
					RHI::MeshPtr ptr = Renderer::GetDriver()->CreateMesh();
					Renderer::GetDriver()->UpdateMesh(ptr, mesh.outVertices, mesh.outIndices);
					outMeshes.emplace_back(ptr);
				}

				const std::string materialsFolder = Utils::GetFileFolder(assetInfo->GetRelativeAssetFilepath()) + "materials/";

				if (assetInfo->ShouldGenerateMaterials())
				{
					for (const auto& material : materials)
					{
						if (AssetInfoPtr materialInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<AssetInfoPtr>(materialsFolder + material.name + ".mat"))
						{
							outMaterials.emplace_back(App::GetSubmodule<MaterialImporter>()->LoadMaterial(materialInfo->GetUID()));
						}
						else
						{
							outMaterials.push_back(RHI::MaterialPtr());
						}
					}
				}
			});

		App::GetSubmodule<JobSystem::Scheduler>()->Run(jobLoad);

		return jobLoad;
	}
	return nullptr;
}