#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>
#include <string>

#include "ModelImporter.h"
#include "AssetRegistry/FileId.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "ModelAssetInfo.h"
#include "Core/Utils.h"
#include "RHI/VertexDescription.h"
#include "RHI/Types.h"
#include "RHI/Renderer.h"
#include "Memory/ObjectAllocator.hpp"

#ifndef TINYGLTF_IMPLEMENTATION
#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>
#endif

using namespace Sailor;

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
		if (bWasExpired && modelAssetInfo->ShouldGenerateMaterials() && modelAssetInfo->GetDefaultMaterials().Num() == 0)
		{
			GenerateMaterialAssets(modelAssetInfo);
			assetInfo->SaveMetaFile();
		}

		if (bWasExpired && modelAssetInfo->GetAnimations().Num() == 0)
		{
			GenerateAnimationAssets(modelAssetInfo);
			assetInfo->SaveMetaFile();
		}
	}
}

void ModelImporter::OnImportAsset(AssetInfoPtr assetInfo)
{
	OnUpdateAssetInfo(assetInfo, true);
}

FileId CreateTextureAsset(const std::string& filepath,
	const std::string& glbFilename,
	uint32_t glbTextureIndex,
	bool bShouldGenerateMips = true,
	RHI::EFormat format = RHI::EFormat::R8G8B8A8_SRGB,
	RHI::ETextureClamping clamping = RHI::ETextureClamping::Repeat,
	RHI::ETextureFiltration filtration = RHI::ETextureFiltration::Linear)
{
	FileId newFileId = FileId::CreateNewFileId();

	YAML::Node newTexture;
	newTexture["fileId"] = newFileId;
	newTexture["filename"] = glbFilename;
	newTexture["glbTextureIndex"] = glbTextureIndex;
	newTexture["bShouldGenerateMips"] = bShouldGenerateMips;
	newTexture["clamping"] = clamping;
	newTexture["filtration"] = filtration;
	newTexture["format"] = format;

	std::ofstream assetFile(filepath);
	assetFile << newTexture;
	assetFile.close();

	return newFileId;
}

FileId CreateAnimationAsset(const std::string& filepath,
	const std::string& glbFilename,
	uint32_t animationIndex,
	uint32_t skinIndex)
{
	FileId newFileId = FileId::CreateNewFileId();

	YAML::Node newAnimation;
	newAnimation["fileId"] = newFileId;
	newAnimation["filename"] = glbFilename;
	newAnimation["animationIndex"] = animationIndex;
	newAnimation["skinIndex"] = skinIndex;

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

	const std::string animationsFolder = AssetRegistry::GetContentFolder() + Utils::GetFileFolder(assetInfo->GetRelativeAssetFilepath());

	for (size_t i = 0; i < gltfModel.animations.size(); ++i)
	{
		const auto& anim = gltfModel.animations[i];
		std::string name = !anim.name.empty() ? anim.name : ("animation" + std::to_string(i));
		FileId id = CreateAnimationAsset(animationsFolder + assetInfo->GetAssetFilename() + "_" + name + ".anim.asset",
			assetInfo->GetAssetFilename(), (uint32_t)i, 0);
		assetInfo->GetAnimations().Add(id);
	}
}

void ModelImporter::GenerateMaterialAssets(ModelAssetInfoPtr assetInfo)
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
		return;
	}

	const std::string texturesFolder = Utils::GetFileFolder(assetInfo->GetRelativeAssetFilepath());

	TVector<MaterialAsset::Data> materials(gltfModel.materials.size());

	for (size_t i = 0; i < gltfModel.materials.size(); ++i)
	{
		const auto& material = gltfModel.materials[i];

		MaterialAsset::Data& data = materials[i];
		data.m_name = !material.name.empty() ? material.name : ("material" + std::to_string(i));

		const std::string materialName = AssetRegistry::GetContentFolder() + texturesFolder + assetInfo->GetAssetFilename() + "_" + data.m_name;

		if (material.pbrMetallicRoughness.baseColorTexture.index != -1)
		{
			data.m_samplers.Add("baseColorSampler",
				CreateTextureAsset(materialName + "_baseColorTexture.png.asset", assetInfo->GetAssetFilename(), material.pbrMetallicRoughness.baseColorTexture.index));
		}

		if (material.normalTexture.index != -1)
		{
			data.m_samplers.Add("normalSampler",
				CreateTextureAsset(materialName + "_normalTexture.png.asset", assetInfo->GetAssetFilename(), material.normalTexture.index, true, RHI::ETextureFormat::R8G8B8A8_UNORM));
		}

		if (material.emissiveTexture.index != -1)
		{
			data.m_samplers.Add("emissiveSampler",
				CreateTextureAsset(materialName + "_emissionTexture.png.asset", assetInfo->GetAssetFilename(), material.emissiveTexture.index));
		}

		if (material.pbrMetallicRoughness.metallicRoughnessTexture.index != -1)
		{
			data.m_samplers.Add("ormSampler",
				CreateTextureAsset(materialName + "_ormTexture.png.asset", assetInfo->GetAssetFilename(), material.pbrMetallicRoughness.metallicRoughnessTexture.index));
		}

		if (material.occlusionTexture.index != -1)
		{
			data.m_samplers.Add("occlusionSampler",
				CreateTextureAsset(materialName + "_occlusionTexture.png.asset", assetInfo->GetAssetFilename(), material.occlusionTexture.index));
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
						data.m_samplers.Add("clearcoatSampler",
							CreateTextureAsset(materialName + "_clearcoatTexture.png.asset", assetInfo->GetAssetFilename(), idx));
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
						data.m_samplers.Add("clearcoatRoughnessSampler",
							CreateTextureAsset(materialName + "_clearcoatRoughnessTexture.png.asset", assetInfo->GetAssetFilename(), idx));
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
						data.m_samplers.Add("clearcoatNormalSampler",
							CreateTextureAsset(materialName + "_clearcoatNormalTexture.png.asset", assetInfo->GetAssetFilename(), idx, true, RHI::ETextureFormat::R8G8B8A8_UNORM));
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
						data.m_samplers.Add("sheenColorSampler",
							CreateTextureAsset(materialName + "_sheenColorTexture.png.asset", assetInfo->GetAssetFilename(), idx));
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
						data.m_samplers.Add("sheenRoughnessSampler",
							CreateTextureAsset(materialName + "_sheenRoughnessTexture.png.asset", assetInfo->GetAssetFilename(), idx));
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

	TVector<FileId> materialFiles;

	for (const auto& material : materials)
	{
		std::string materialsFolder = AssetRegistry::GetContentFolder() + texturesFolder + "materials/";
		std::filesystem::create_directory(materialsFolder);

		FileId materialFileId = App::GetSubmodule<MaterialImporter>()->CreateMaterialAsset(materialsFolder + material.m_name + ".mat", material);
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
		SAILOR_PROFILE_TEXT(assetInfo->GetAssetFilepath().c_str());

		ModelPtr model = ModelPtr::Make(m_allocator, uid);

		// The way to drop qualifiers inside lambda
		auto& boundsSphere = model->m_boundsSphere;
		auto& boundsAabb = model->m_boundsAabb;

		struct Data
		{
			TVector<MeshContext> m_parsedMeshes;
			TVector<glm::mat4> m_inverseBind;
			bool m_bIsImported = false;
		};

		promise = Tasks::CreateTaskWithResult<TSharedPtr<Data>>("Load model",
			[model, assetInfo, &boundsAabb, &boundsSphere]()
			{
				TSharedPtr<Data> res = TSharedPtr<Data>::Make();
				res->m_bIsImported = ImportModel(assetInfo, res->m_parsedMeshes, boundsAabb, boundsSphere, res->m_inverseBind);
				return res;
			})->Then<ModelPtr>([model](TSharedPtr<Data> data) mutable
				{
					if (data->m_bIsImported)
					{
						for (const auto& mesh : data->m_parsedMeshes)
						{
							RHI::RHIMeshPtr ptr = RHI::Renderer::GetDriver()->CreateMesh();
							ptr->m_vertexDescription = RHI::Renderer::GetDriver()->GetOrAddVertexDescription<RHI::VertexP3N3T3B3UV2C4I4W4>();
							ptr->m_bounds = mesh.bounds;
							RHI::Renderer::GetDriver()->UpdateMesh(ptr,
								mesh.outVertices.GetData(), sizeof(RHI::VertexP3N3T3B3UV2C4I4W4) * mesh.outVertices.Num(),
								mesh.outIndices.GetData(), sizeof(uint32_t) * mesh.outIndices.Num());

							model->m_meshes.Emplace(ptr);
						}

						model->m_inverseBind = std::move(data->m_inverseBind);
						model->Flush();
					}
					return model;
				}, "Update RHI Meshes", EThreadType::RHI)->ToTaskWithResult();

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

bool ModelImporter::LoadModel_Immediate(FileId uid, ModelPtr& outModel)
{
	SAILOR_PROFILE_FUNCTION();

	auto task = LoadModel(uid, outModel);
	task->Wait();
	return task->GetResult().IsValid();
}

void CalculateTangentBitangent(vec3& outTangent, vec3& outBitangent, const vec3* vert, const vec2* uv)
{
	vec3 edge1 = vert[1] - vert[0];
	vec3 edge2 = vert[2] - vert[0];

	vec2 deltaUV1 = uv[1] - uv[0];
	vec2 deltaUV2 = uv[2] - uv[0];

	float denominator = deltaUV1.x * deltaUV2.y - deltaUV2.x * deltaUV1.y;
	if (abs(denominator) < 1e-6f)
	{
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
		meshContext.outVertices[vertexOffset + i].m_tangent = glm::normalize(tangents[i]);
		meshContext.outVertices[vertexOffset + i].m_bitangent = glm::normalize(bitangents[i]);
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
	tinygltf::Model gltfModel;
	tinygltf::TinyGLTF loader;
	std::string err;
	std::string warn;

	const bool bGltfParsed = (Utils::GetFileExtension(assetInfo->GetAssetFilepath().c_str()) == "glb") ?
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

	const float unitScale = assetInfo->GetUnitScale();

	outBoundsAabb.m_max = glm::vec3(std::numeric_limits<float>::min());
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
	TVector<MeshContext> batchedMeshContexts(std::max(1ull, gltfModel.materials.size()));

	for (const auto& mesh : gltfModel.meshes)
	{
		for (const auto& primitive : mesh.primitives)
		{
			MeshContext* pMeshContext = nullptr;
			uint32_t startIndex = 0;
			uint32_t indicesStart = 0;

			if (assetInfo->ShouldBatchByMaterial())
			{
				pMeshContext = &batchedMeshContexts[std::max(primitive.material, 0)];
				startIndex = (uint32_t)pMeshContext->outVertices.Num();
				indicesStart = (uint32_t)pMeshContext->outIndices.Num();
			}
			else
			{
				outParsedMeshes.Add(MeshContext());
				pMeshContext = &(*outParsedMeshes.Last());
				indicesStart = (uint32_t)pMeshContext->outIndices.Num();
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

	if (assetInfo->ShouldBatchByMaterial())
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
}