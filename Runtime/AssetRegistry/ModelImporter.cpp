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

void ModelImporter::OnAssetInfoUpdated(AssetInfoPtr assetInfo)
{
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

				if (assetInfo->ShouldGenerateMaterials())
				{
					auto texturesFolder = Utils::GetFileFolder(assetInfo->GetRelativeAssetFilepath());
					for (const auto& material : materials)
					{
						MaterialAsset::Data data;
						data.m_shader = AssetRegistry::GetInstance()->GetAssetInfoPtr("Shaders/Simple.shader")->GetUID();

						glm::vec4 diffuse, ambient, emission, specular;
						memcpy(&diffuse, material.diffuse, 3 * sizeof(tinyobj::real_t));
						memcpy(&ambient, material.ambient, 3 * sizeof(tinyobj::real_t));
						memcpy(&emission, material.emission, 3 * sizeof(tinyobj::real_t));
						memcpy(&specular, material.specular, 3 * sizeof(tinyobj::real_t));
						data.m_uniformsVec4.push_back({ "diffuse", diffuse });
						data.m_uniformsVec4.push_back({ "ambient", ambient });
						data.m_uniformsVec4.push_back({ "emission", emission });
						data.m_uniformsVec4.push_back({ "specular", specular });

						if (auto assetInfo = AssetRegistry::GetInstance()->GetAssetInfoPtr(texturesFolder + material.diffuse_texname))
						{
							UID diffuseSampler = assetInfo->GetUID();
							data.m_samplers.push_back(MaterialAsset::SamplerEntry("diffuseSampler", diffuseSampler));
						}

						if (auto assetInfo = AssetRegistry::GetInstance()->GetAssetInfoPtr(texturesFolder + material.ambient_texname))
						{
							UID ambientSampler = assetInfo->GetUID();
							data.m_samplers.push_back(MaterialAsset::SamplerEntry("ambientSampler", ambientSampler));
						}

						if (auto assetInfo = AssetRegistry::GetInstance()->GetAssetInfoPtr(texturesFolder + material.normal_texname))
						{
							UID normalSampler = assetInfo->GetUID();
							data.m_samplers.push_back(MaterialAsset::SamplerEntry("normalSampler", normalSampler));
						}

						std::string materialsFolder = AssetRegistry::ContentRootFolder + texturesFolder + "materials/";
						std::filesystem::create_directory(materialsFolder);

						MaterialImporter::CreateMaterialAsset(materialsFolder + material.name + ".mat", std::move(data));
					}
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