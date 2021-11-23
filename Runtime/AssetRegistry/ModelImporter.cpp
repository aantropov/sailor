#include "ModelImporter.h"
#include "UID.h"
#include "AssetRegistry.h"
#include "MaterialImporter.h"
#include "ModelAssetInfo.h"
#include "Utils.h"
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

void ModelImporter::Initialize()
{
	SAILOR_PROFILE_FUNCTION();

	m_pInstance = new ModelImporter();
	ModelAssetInfoHandler::GetInstance()->Subscribe(m_pInstance);
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

		data.m_shader = AssetRegistry::GetInstance()->GetOrLoadAsset("Shaders/Simple.shader");

		glm::vec4 diffuse, ambient, emission, specular;
		memcpy(&diffuse, material.diffuse, 3 * sizeof(tinyobj::real_t));
		memcpy(&ambient, material.ambient, 3 * sizeof(tinyobj::real_t));
		memcpy(&emission, material.emission, 3 * sizeof(tinyobj::real_t));
		memcpy(&specular, material.specular, 3 * sizeof(tinyobj::real_t));
		data.m_uniformsVec4.push_back({ "diffuse", diffuse });
		data.m_uniformsVec4.push_back({ "ambient", ambient });
		data.m_uniformsVec4.push_back({ "emission", emission });
		data.m_uniformsVec4.push_back({ "specular", specular });

		if (!material.diffuse_texname.empty())
		{
			data.m_samplers.push_back(MaterialAsset::SamplerEntry("diffuseSampler", AssetRegistry::GetInstance()->GetOrLoadAsset(texturesFolder + material.diffuse_texname)));
		}

		if (!material.ambient_texname.empty())
		{
			data.m_samplers.push_back(MaterialAsset::SamplerEntry("ambientSampler", AssetRegistry::GetInstance()->GetOrLoadAsset(texturesFolder + material.ambient_texname)));
		}

		if (!material.normal_texname.empty())
		{
			data.m_samplers.push_back(MaterialAsset::SamplerEntry("normalSampler", AssetRegistry::GetInstance()->GetOrLoadAsset(texturesFolder + material.normal_texname)));
		}

		std::string materialsFolder = AssetRegistry::ContentRootFolder + texturesFolder + "materials/";
		std::filesystem::create_directory(materialsFolder);

		MaterialImporter::CreateMaterialAsset(materialsFolder + material.name + ".mat", std::move(data));
	}
}

TSharedPtr<JobSystem::Job> ModelImporter::LoadModel(UID uid, std::vector<RHI::Vertex>& outVertices, std::vector<uint32_t>& outIndices)
{
	SAILOR_PROFILE_FUNCTION();

	if (ModelAssetInfoPtr assetInfo = AssetRegistry::GetInstance()->GetAssetInfoPtr<ModelAssetInfoPtr>(uid))
	{
		auto jobLoad = JobSystem::Scheduler::CreateJob("Check unique vertices",
			[&outIndices, &outVertices, assetInfo]()
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

				std::unordered_map<RHI::Vertex, uint32_t> uniqueVertices{};

				for (const auto& shape : shapes)
				{
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

						if (uniqueVertices.count(vertex) == 0)
						{
							uniqueVertices[vertex] = static_cast<uint32_t>(outVertices.size());
							outVertices.push_back(vertex);
						}

						outIndices.push_back(uniqueVertices[vertex]);
					}
				}
			});

		JobSystem::Scheduler::GetInstance()->Run(jobLoad);

		return jobLoad;
	}
	return nullptr;
}