#include "Raytracing.h"
#include "Tasks/Scheduler.h"
#include "Containers/Vector.h"
#include "Containers/Set.h"
#include "Containers/Map.h"
#include "Containers/List.h"
#include "Containers/Octree.h"
#include "Memory/MemoryBlockAllocator.hpp"
#include "Core/LogMacros.h"
#include "Core/Utils.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "Memory/UniquePtr.hpp"
#include "Containers/Containers.h"
#include "glm/glm/glm.hpp"
#include "Math/Math.h"
#include "Math/Bounds.h"
#include "glm/glm/gtc/random.hpp"

#include "stb/stb_image.h"

#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define __STDC_LIB_EXT1__
#include "stb/stb_image_write.h"
#endif 

#include "assimp/scene.h"
#include "assimp/postprocess.h"
#include "assimp/Importer.hpp"
#include "assimp/DefaultLogger.hpp"
#include "assimp/LogStream.hpp"

#include "nlohmann_json/include/nlohmann/json.hpp"

using namespace Sailor;
using namespace Sailor::Math;

float BVH::FindBestSplitPlane(const BVHNode& node, const TVector<Math::Triangle>& tris, int32_t& outAxis, float& outSplitPos) const
{
	SAILOR_PROFILE_FUNCTION();

	struct Bin { Math::AABB m_bounds{}; int m_triCount = 0; };

	const uint32_t NumBins = 8;

	float bestCost = std::numeric_limits<float>::max();
	for (uint32_t a = 0; a < 3; a++)
	{
		float boundsMin = std::numeric_limits<float>::max();
		float boundsMax = -300000.0f;

		for (uint32_t i = 0; i < node.m_triCount; i++)
		{
			const Math::Triangle& triangle = tris[m_triIdx[node.m_leftFirst + i]];
			boundsMin = std::min(boundsMin, triangle.m_centroid[a]);
			boundsMax = std::max(boundsMax, triangle.m_centroid[a]);
		}

		if (boundsMin == boundsMax)
		{
			continue;
		}

		Bin bin[NumBins];
		float scale = NumBins / (boundsMax - boundsMin);
		for (uint i = 0; i < node.m_triCount; i++)
		{
			const Math::Triangle& triangle = tris[m_triIdx[node.m_leftFirst + i]];
			int32_t binIdx = std::min((int32_t)NumBins - 1,
				(int32_t)((triangle.m_centroid[a] - boundsMin) * scale));
			bin[binIdx].m_triCount++;
			bin[binIdx].m_bounds.Extend(triangle.m_vertices[0]);
			bin[binIdx].m_bounds.Extend(triangle.m_vertices[1]);
			bin[binIdx].m_bounds.Extend(triangle.m_vertices[2]);
		}

		float leftArea[NumBins - 1];
		float rightArea[NumBins - 1];
		int32_t leftCount[NumBins - 1];
		int32_t rightCount[NumBins - 1];

		Math::AABB leftBox;
		Math::AABB rightBox;
		int32_t leftSum = 0;
		int32_t rightSum = 0;
		for (int32_t i = 0; i < NumBins - 1; i++)
		{
			leftSum += bin[i].m_triCount;
			leftCount[i] = leftSum;
			leftBox.Extend(bin[i].m_bounds);
			leftArea[i] = leftBox.Area();
			rightSum += bin[NumBins - 1 - i].m_triCount;
			rightCount[NumBins - 2 - i] = rightSum;
			rightBox.Extend(bin[NumBins - 1 - i].m_bounds);
			rightArea[NumBins - 2 - i] = rightBox.Area();
		}

		scale = (boundsMax - boundsMin) / NumBins;
		for (int32_t i = 0; i < NumBins - 1; i++)
		{
			float planeCost = leftCount[i] * leftArea[i] + rightCount[i] * rightArea[i];
			if (planeCost < bestCost)
			{
				outAxis = a;
				outSplitPos = boundsMin + scale * (i + 1);
				bestCost = planeCost;
			}
		}
	}
	return bestCost;
}

float BVH::EvaluateSAH(const BVHNode& node, const TVector<Math::Triangle>& tris, int32_t axis, float pos) const
{
	SAILOR_PROFILE_FUNCTION();

	Math::AABB leftBox;
	Math::AABB rightBox;

	int32_t leftCount = 0;
	int32_t rightCount = 0;

	for (uint i = 0; i < node.m_triCount; i++)
	{
		const Math::Triangle& triangle = tris[m_triIdx[node.m_leftFirst + i]];
		if (triangle.m_centroid[axis] < pos)
		{
			leftCount++;
			leftBox.Extend(triangle.m_vertices[0]);
			leftBox.Extend(triangle.m_vertices[1]);
			leftBox.Extend(triangle.m_vertices[2]);
		}
		else
		{
			rightCount++;
			rightBox.Extend(triangle.m_vertices[0]);
			rightBox.Extend(triangle.m_vertices[1]);
			rightBox.Extend(triangle.m_vertices[2]);
		}
	}
	float cost = leftCount * leftBox.Area() + rightCount * rightBox.Area();
	return cost > 0 ? cost : 1e30f;
}

bool BVH::IntersectBVH(const Math::Ray& ray, Math::RaycastHit& outResult, const uint nodeIdx, float maxRayLength) const
{
	SAILOR_PROFILE_FUNCTION();

	const BVHNode* node = &m_nodes[m_rootNodeIdx], * stack[64];
	uint stackPtr = 0;
	Math::RaycastHit res{};
	while (1)
	{
		if (node->IsLeaf())
		{
			for (uint i = 0; i < node->m_triCount; i++)
			{
				if (Math::IntersectRayTriangle(ray, m_triangles[node->m_leftFirst + i], res, maxRayLength))
				{
					outResult = res;
					outResult.m_triangleIndex = m_triIdxMapping[node->m_leftFirst + i];

					maxRayLength = std::min(maxRayLength, res.m_rayLenght);
				}
			}
			if (stackPtr == 0)
			{
				break;
			}
			else
			{
				node = stack[--stackPtr];
			}

			continue;
		}

		const BVH::BVHNode* child1 = &m_nodes[node->m_leftFirst];
		const BVH::BVHNode* child2 = &m_nodes[node->m_leftFirst + 1];

		float dist1 = IntersectRayAABB(ray, child1->m_aabbMin, child1->m_aabbMax, maxRayLength);
		float dist2 = IntersectRayAABB(ray, child2->m_aabbMin, child2->m_aabbMax, maxRayLength);

		if (dist1 > dist2)
		{
			swap(dist1, dist2);
			swap(child1, child2);
		}

		if (dist1 == std::numeric_limits<float>::max())
		{
			if (stackPtr == 0)
			{
				break;
			}
			else
			{
				node = stack[--stackPtr];
			}
		}
		else
		{
			node = child1;
			if (dist2 != std::numeric_limits<float>::max())
			{
				stack[stackPtr++] = child2;
			}
		}
	}

	return outResult.HasIntersection();
}

void BVH::UpdateNodeBounds(uint32_t nodeIdx, const TVector<Math::Triangle>& tris)
{
	SAILOR_PROFILE_FUNCTION();

	BVH::BVHNode& node = m_nodes[nodeIdx];

	node.m_aabbMin = vec3(1e30f);
	node.m_aabbMax = vec3(-1e30f);

	for (uint first = node.m_leftFirst, i = 0; i < node.m_triCount; i++)
	{
		uint leafTriIdx = m_triIdx[first + i];
		const Triangle& leafTri = tris[leafTriIdx];
		node.m_aabbMin = glm::min(node.m_aabbMin, leafTri.m_vertices[0]);
		node.m_aabbMin = glm::min(node.m_aabbMin, leafTri.m_vertices[1]);
		node.m_aabbMin = glm::min(node.m_aabbMin, leafTri.m_vertices[2]);
		node.m_aabbMax = glm::max(node.m_aabbMax, leafTri.m_vertices[0]);
		node.m_aabbMax = glm::max(node.m_aabbMax, leafTri.m_vertices[1]);
		node.m_aabbMax = glm::max(node.m_aabbMax, leafTri.m_vertices[2]);
	}
}

void BVH::Subdivide(uint32_t nodeIdx, const TVector<Math::Triangle>& tris)
{
	SAILOR_PROFILE_FUNCTION();

	// terminate recursion
	BVH::BVHNode& node = m_nodes[nodeIdx];

	if (node.m_triCount <= 4)
	{
		return;
	}

	int32_t axis{};
	float splitPos{};
	float splitCost = FindBestSplitPlane(node, tris, axis, splitPos);

	float nosplitCost = node.CalculateCost();
	if (splitCost >= nosplitCost)
	{
		return;
	}

	// in-place partition
	int32_t i = node.m_leftFirst;
	int32_t j = i + node.m_triCount - 1;

	while (i <= j)
	{
		if (tris[m_triIdx[i]].m_centroid[axis] < splitPos)
		{
			i++;
		}
		else
		{
			swap(m_triIdx[i], m_triIdx[j--]);
		}
	}

	// abort split if one of the sides is empty
	uint32_t leftCount = i - node.m_leftFirst;

	if (leftCount == 0 || leftCount == node.m_triCount)
	{
		return;
	}

	// create child nodes
	uint32_t leftChildIdx = m_nodesUsed++;
	uint32_t rightChildIdx = m_nodesUsed++;

	m_nodes[leftChildIdx].m_leftFirst = node.m_leftFirst;
	m_nodes[leftChildIdx].m_triCount = leftCount;
	m_nodes[rightChildIdx].m_leftFirst = i;
	m_nodes[rightChildIdx].m_triCount = node.m_triCount - leftCount;

	node.m_leftFirst = leftChildIdx;
	node.m_triCount = 0;

	UpdateNodeBounds(leftChildIdx, tris);
	UpdateNodeBounds(rightChildIdx, tris);

	Subdivide(leftChildIdx, tris);
	Subdivide(rightChildIdx, tris);
}

void BVH::BuildBVH(const TVector<Math::Triangle>& tris)
{
	SAILOR_PROFILE_FUNCTION();

	check(tris.Num() * 2 - 1 == m_nodes.Num());

	for (uint32_t i = 0; i < m_nodes.Num(); i++)
	{
		m_triIdx[i] = i;
	}

	BVHNode& root = m_nodes[m_rootNodeIdx];
	root.m_leftFirst = 0;
	root.m_triCount = (uint32_t)tris.Num();

	UpdateNodeBounds(m_rootNodeIdx, tris);
	Subdivide(m_rootNodeIdx, tris);

	SAILOR_PROFILE_BLOCK("Copy/Locality triangle data");
	// Cache locality
	m_triangles.Reserve(tris.Num());
	m_triIdxMapping.AddDefault(tris.Num());

	// TODO: Parallilize
	for (uint32_t i = 0; i < m_nodes.Num(); i++)
	{
		if (m_nodes[i].IsLeaf())
		{
			const uint32_t triIndex = m_nodes[i].m_leftFirst;
			m_nodes[i].m_leftFirst = (int32_t)m_triangles.Num();

			if (m_triangles.Num() == 1)
			{
				const uint32_t triId = m_triIdx[triIndex];
				m_triIdxMapping[m_triangles.Num()] = triId;
				m_triangles.Add(tris[triId]);
				continue;
			}

			SAILOR_PROFILE_BLOCK("Sort Triangles by area");
			TVector<uint32_t> sorted(m_nodes[i].m_triCount);
			for (uint32_t j = 0; j < m_nodes[i].m_triCount; j++)
			{
				sorted[j] = m_triIdx[triIndex + j];
			}

			sorted.Sort([&](const auto& lhs, const auto& rhs)
				{
					return tris[lhs].SquareArea() > tris[rhs].SquareArea();
				});
			SAILOR_PROFILE_END_BLOCK();

			SAILOR_PROFILE_BLOCK("Copy data");
			for (uint32_t j = 0; j < m_nodes[i].m_triCount; j++)
			{
				const uint32_t triId = sorted[j];// m_triIdx[triIndex + j];
				m_triIdxMapping[m_triangles.Num()] = triId;
				m_triangles.Add(tris[triId]);
			}
			SAILOR_PROFILE_END_BLOCK();
		}
	}
	SAILOR_PROFILE_END_BLOCK();
}

namespace Sailor::Internal
{
	void WriteImage(const char* outputFileName, const TVector<u8vec3>& data, uint32_t width, uint32_t height)
	{
		SAILOR_PROFILE_FUNCTION();
		const uint32_t Channels = 3;

		if (!stbi_write_png(outputFileName, width, height, Channels, &data[0], width * Channels))
		{
			SAILOR_LOG("Raytracing WriteImage error");
		}
	}
}

void ProcessMesh_Assimp(aiMesh* mesh, TVector<Triangle>& outScene, const aiScene* scene)
{
	SAILOR_PROFILE_FUNCTION();

	check(mesh->HasPositions());
	check(mesh->HasNormals());

	const size_t startIndex = outScene.Num();

	outScene.AddDefault(mesh->mNumFaces);
	for (uint32_t i = 0; i < mesh->mNumFaces; i++)
	{
		aiFace face = mesh->mFaces[i];
		if (face.mNumIndices != 3)
		{
			continue;
		}

		Math::Triangle& tri = outScene[startIndex + i];

		// TODO: use pointers instead of memcpy
		memcpy(&tri.m_vertices[0], &mesh->mVertices[face.mIndices[0]], sizeof(float) * 3);
		memcpy(&tri.m_vertices[1], &mesh->mVertices[face.mIndices[1]], sizeof(float) * 3);
		memcpy(&tri.m_vertices[2], &mesh->mVertices[face.mIndices[2]], sizeof(float) * 3);

		memcpy(&tri.m_normals[0], &mesh->mNormals[face.mIndices[0]], sizeof(float) * 3);
		memcpy(&tri.m_normals[1], &mesh->mNormals[face.mIndices[1]], sizeof(float) * 3);
		memcpy(&tri.m_normals[2], &mesh->mNormals[face.mIndices[2]], sizeof(float) * 3);

		tri.m_centroid = (tri.m_vertices[0] + tri.m_vertices[1] + tri.m_vertices[2]) * 0.333f;

		if (mesh->HasTextureCoords(0))
		{
			memcpy(&tri.m_uvs[0], &mesh->mTextureCoords[0][face.mIndices[0]], sizeof(float) * 2);
			memcpy(&tri.m_uvs[1], &mesh->mTextureCoords[0][face.mIndices[1]], sizeof(float) * 2);
			memcpy(&tri.m_uvs[2], &mesh->mTextureCoords[0][face.mIndices[2]], sizeof(float) * 2);
		}

		if (mesh->HasTangentsAndBitangents())
		{
			memcpy(&tri.m_tangent[0], &mesh->mTangents[face.mIndices[0]], sizeof(float) * 3);
			memcpy(&tri.m_tangent[1], &mesh->mTangents[face.mIndices[1]], sizeof(float) * 3);
			memcpy(&tri.m_tangent[2], &mesh->mTangents[face.mIndices[2]], sizeof(float) * 3);

			memcpy(&tri.m_bitangent[0], &mesh->mBitangents[face.mIndices[0]], sizeof(float) * 3);
			memcpy(&tri.m_bitangent[1], &mesh->mBitangents[face.mIndices[1]], sizeof(float) * 3);
			memcpy(&tri.m_bitangent[2], &mesh->mBitangents[face.mIndices[2]], sizeof(float) * 3);
		}

		tri.m_materialIndex = mesh->mMaterialIndex;
		//memcpy(&mesh->mVertices[face.mIndices[0]], tri.m_vertices, sizeof(float) * 3);
	}
}

void ProcessNode_Assimp(TVector<Triangle>& outScene, aiNode* node, const aiScene* scene)
{
	SAILOR_PROFILE_FUNCTION();

	for (uint32_t i = 0; i < node->mNumMeshes; i++)
	{
		aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
		ProcessMesh_Assimp(mesh, outScene, scene);
	}

	for (uint32_t i = 0; i < node->mNumChildren; i++)
	{
		ProcessNode_Assimp(outScene, node->mChildren[i], scene);
	}
}

Raytracing::Texture2D::~Texture2D() {}

void Raytracing::Run()
{
	SAILOR_PROFILE_FUNCTION();

	Utils::Timer raytracingTimer;
	raytracingTimer.Start();

	const uint32_t GroupSize = 32;

	const char* outputFile = "output.png";
	const std::filesystem::path sceneFile = "../Content/Models/Sponza/Sponza.obj";
	//const std::filesystem::path sceneFile = "../Content/Models/Duck/Duck.gltf";
	//const std::filesystem::path sceneFile = "../Content/Models/BoxTextured/BoxTextured.gltf";
	//const std::filesystem::path sceneFile = "../Content/Models/Triangle/Triangle.gltf";

	Assimp::Importer importer;

	const unsigned int DefaultImportFlags_Assimp =
		aiProcess_CalcTangentSpace |
		aiProcess_Triangulate |
		aiProcess_FlipUVs |
		aiProcess_SortByPType |
		aiProcess_PreTransformVertices |
		aiProcess_GenNormals |
		aiProcess_GenUVCoords |
		aiProcess_Debone |
		//aiProcess_ValidateDataStructure |
		aiProcess_FindDegenerates |
		//aiProcess_ImproveCacheLocality |
		//aiProcess_FindInvalidData |
		//aiProcess_FlipWindingOrder |
		0;

	const auto scene = importer.ReadFile(sceneFile.string().c_str(), DefaultImportFlags_Assimp);

	if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
	{
		SAILOR_LOG("%s", importer.GetErrorString());
		return;
	}

	ensure(scene->HasCameras(), "Scene %s has no Cameras!", sceneFile.string().c_str());

	float aspectRatio = scene->HasCameras() ? scene->mCameras[0]->mAspect : 16.0f / 9.0f;
	const uint32_t width = 1920;
	const uint32_t height = static_cast<uint32_t>(width / aspectRatio);
	const float hFov = scene->HasCameras() ? scene->mCameras[0]->mHorizontalFOV : glm::radians(80.0f);
	const float vFov = 2.0f * atan(tan(hFov / 2.0f) * (1.0f / aspectRatio));

	const float zMin = scene->HasCameras() ? scene->mCameras[0]->mClipPlaneNear : 0.01f;
	const float zMax = scene->HasCameras() ? scene->mCameras[0]->mClipPlaneFar : 1000.0f;

	// Camera
	const mat4 projectionMatrix = transpose(glm::perspectiveFovRH(vFov, (float)width, (float)height, zMin, zMax));
	mat4 viewMatrix{ 1 };

	auto cameraPos = vec3(0, 500, 0);
	auto cameraUp = normalize(vec3(0, 1, 0));
	//auto cameraForward = normalize(-cameraPos);
	auto cameraForward = vec3(1, 0, 0);

	auto axis = cross(cameraForward, cameraUp);
	cameraUp = cross(axis, cameraForward);

	// View
	if (scene->HasCameras())
	{
		const auto& aiCamera = scene->mCameras[0];

		aiNode* rootNode = scene->mRootNode;
		aiNode* cameraNode = rootNode->FindNode(aiCamera->mName);

		mat4 cameraTransformationMatrix{};
		::memcpy(&cameraTransformationMatrix, &cameraNode->mTransformation, sizeof(float) * 16);

		vec4 camPos{ 0,0,0,1 };
		::memcpy(&camPos, &aiCamera->mPosition, sizeof(float) * 3);
		camPos = camPos * cameraTransformationMatrix;
		camPos /= camPos.w;
		cameraPos = camPos.xyz;

		::memcpy(&cameraForward, &aiCamera->mLookAt, sizeof(float) * 3);
		cameraForward = vec3(vec4(cameraForward, 0) * cameraTransformationMatrix);
		cameraForward = normalize(cameraForward);

		::memcpy(&cameraUp, &aiCamera->mUp, sizeof(float) * 3);
		cameraUp = normalize(cameraUp);

		viewMatrix = transpose(glm::lookAtRH(vec3(camPos.xyz), vec3(camPos.xyz) + cameraForward, cameraUp));
	}

	const auto cameraRight = normalize(cross(cameraForward, cameraUp));

	uint32_t numFaces = 0;
	for (uint32_t i = 0; i < scene->mNumMeshes; i++)
	{
		numFaces += scene->mMeshes[i]->mNumFaces;
	}

	m_triangles.Reserve(numFaces);
	ProcessNode_Assimp(m_triangles, scene->mRootNode, scene);

	{
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

		auto TraceUsedTextures_Assimp = [](aiMaterial* mat, aiTextureType type)
		{
			TVector<std::string> textures;
			for (uint32_t i = 0; i < mat->GetTextureCount(type); i++)
			{
				aiString str;
				mat->GetTexture(type, i, &str);
				textures.Add(str.C_Str());
			}
			return textures;
		};

		std::function<void(uint32_t, const char*)> LoadTexture =
			[&m_textures = m_textures,
			sceneFile = sceneFile,
			&scene = scene](uint32_t i, const char* filename) mutable
		{
			m_textures[i] = TSharedPtr<Texture2D>::Make();

			const auto& texture = scene->mTextures[i];
			sceneFile.replace_filename(filename);

			int32_t texChannels = 0;
			if (stbi_uc* pixels = stbi_load(sceneFile.string().c_str(), &m_textures[i]->m_width, &m_textures[i]->m_height, &texChannels, STBI_rgb_alpha))
			{
				//m_textures[i]->m_data = (u8*)pixels;
				m_textures[i]->Initialize<u8vec4>((u8*)pixels);
				stbi_image_free(pixels);
			}
		};

		TVector<Tasks::ITaskPtr> loadTexturesTasks;
		m_materials.Resize(scene->mNumMaterials);
		m_textures.Resize(1000);

		uint32_t textureIndex = 0;
		for (uint32_t i = 0; i < scene->mNumMaterials; i++)
		{
			auto& material = m_materials[i];
			const auto& aiMaterial = scene->mMaterials[i];

			const TVector<std::string> diffuseMaps = TraceUsedTextures_Assimp(aiMaterial, aiTextureType_DIFFUSE);
			const TVector<std::string> specularMaps = TraceUsedTextures_Assimp(aiMaterial, aiTextureType_SPECULAR);
			const TVector<std::string> ambientMaps = TraceUsedTextures_Assimp(aiMaterial, aiTextureType_AMBIENT);
			const TVector<std::string> normalMaps = TraceUsedTextures_Assimp(aiMaterial, aiTextureType_NORMALS);
			const TVector<std::string> emissionMaps = TraceUsedTextures_Assimp(aiMaterial, aiTextureType_EMISSIVE);

			for (uint32_t i = 0; i < aiMaterial->mNumProperties; i++)
			{
				const auto property = aiMaterial->mProperties[i];
				if (property->mType == aiPTI_Float && property->mDataLength >= sizeof(float) * 3)
				{
					if (FillData(property, std::string("diffuse"), &material.m_diffuse) ||
						FillData(property, std::string("ambient"), &material.m_ambient) ||
						FillData(property, std::string("emission"), &material.m_emission) ||
						FillData(property, std::string("specular"), &material.m_specular))
					{
						continue;
					}
				}
			}

			if (!diffuseMaps.IsEmpty())
			{
				Tasks::ITaskPtr task = Tasks::CreateTask("Load Texture", [=]() { LoadTexture(textureIndex, diffuseMaps[0].c_str()); })->Run();
				loadTexturesTasks.Emplace(std::move(task));
				material.m_diffuseIndex = textureIndex;
				textureIndex++;
			}

			if (!ambientMaps.IsEmpty())
			{
				Tasks::ITaskPtr task = Tasks::CreateTask("Load Texture", [=]() { LoadTexture(textureIndex, ambientMaps[0].c_str()); })->Run();
				loadTexturesTasks.Emplace(std::move(task));
				material.m_ambientIndex = textureIndex;
				textureIndex++;
			}

			if (!normalMaps.IsEmpty())
			{
				Tasks::ITaskPtr task = Tasks::CreateTask("Load Texture", [=]() { LoadTexture(textureIndex, normalMaps[0].c_str()); })->Run();
				loadTexturesTasks.Emplace(std::move(task));
				material.m_normalIndex = textureIndex;
				textureIndex++;
			}

			if (!specularMaps.IsEmpty())
			{
				Tasks::ITaskPtr task = Tasks::CreateTask("Load Texture", [=]() { LoadTexture(textureIndex, specularMaps[0].c_str()); })->Run();
				loadTexturesTasks.Emplace(std::move(task));
				material.m_specularIndex = textureIndex;
				textureIndex++;
			}

			if (!emissionMaps.IsEmpty())
			{
				Tasks::ITaskPtr task = Tasks::CreateTask("Load Texture", [=]() { LoadTexture(textureIndex, emissionMaps[0].c_str()); })->Run();
				loadTexturesTasks.Emplace(std::move(task));
				material.m_emissionIndex = textureIndex;
				textureIndex++;
			}
		}

		for (auto& task : loadTexturesTasks)
		{
			task->Wait();
		}
	}

	BVH bvh(numFaces);
	bvh.BuildBVH(m_triangles);

	SAILOR_PROFILE_BLOCK("Viewport Calcs");

	TVector<u8vec3> output(width * height);

	quat halfViewH = glm::angleAxis(hFov * 0.5f, cameraUp);
	auto axis1 = normalize(halfViewH * cameraForward);
	auto axis2 = normalize(cross(axis1, cameraUp));
	quat halfViewV = glm::angleAxis(vFov * 0.5f, -axis2);

	auto viewportUpperLeft = cameraPos + halfViewH * inverse(halfViewV) * cameraForward;
	auto viewportUpperRight = cameraPos + inverse(halfViewH) * inverse(halfViewV) * cameraForward;
	auto viewportBottomLeft = cameraPos + halfViewH * halfViewV * cameraForward;
	auto viewportBottomRight = cameraPos + inverse(halfViewH) * halfViewV * cameraForward;

	SAILOR_PROFILE_END_BLOCK();

	// Raytracing
	{
		SAILOR_PROFILE_BLOCK("Prepare raytracing tasks");

		TVector<Tasks::ITaskPtr> tasks;
		TVector<Tasks::ITaskPtr> tasksThisThread;

		tasks.Reserve((height * width) / (GroupSize * GroupSize));
		tasksThisThread.Reserve((height * width) / (GroupSize * GroupSize) / 32);

		for (uint32_t y = 0; y < height; y += GroupSize)
		{
			for (uint32_t x = 0; x < width; x += GroupSize)
			{
				auto task = Tasks::CreateTask("Calculate raytracing",
					[=,
					&output,
					&bvh,
					this]() mutable
					{
						Ray ray;
						vec2 prevUV = vec2(1000.0f);
						u8vec4 diffuseSample{};
						uint32_t prevMatIndex = -1;
						const float VariableShaderRate = 0.25f;
						ray.SetOrigin(cameraPos);

						for (uint32_t v = 0; v < GroupSize; v++)
						{
							float tv = (y + v) / (float)height;
							for (uint32_t u = 0; u < GroupSize; u++)
							{
								SAILOR_PROFILE_BLOCK("Raycasting");

								const uint32_t index = (y + v) * width + (x + u);
								float tu = (x + u) / (float)width;

								const vec3 midTop = viewportUpperLeft + (viewportUpperRight - viewportUpperLeft) * tu;
								const vec3 midBottom = viewportBottomLeft + (viewportBottomRight - viewportBottomLeft) * tu;
								ray.SetDirection(glm::normalize(midTop + (midBottom - midTop) * tv - cameraPos));

								RaycastHit hit;
								if (bvh.IntersectBVH(ray, hit, 0))
								{
									SAILOR_PROFILE_BLOCK("Sampling");

									const Math::Triangle& tri = m_triangles[hit.m_triangleIndex];

									vec3 normal = 255.0f * (0.5f + 0.5f * vec3(hit.m_barycentricCoordinate.x * tri.m_normals[0] +
										hit.m_barycentricCoordinate.y * tri.m_normals[1] +
										hit.m_barycentricCoordinate.z * tri.m_normals[2]));

									vec2 uv = hit.m_barycentricCoordinate.x * tri.m_uvs[0] +
										hit.m_barycentricCoordinate.y * tri.m_uvs[1] +
										hit.m_barycentricCoordinate.z * tri.m_uvs[2];

									const auto delta = prevUV - uv;
									const float dUv = dot(delta, delta);

									const auto& material = m_materials[tri.m_materialIndex];
									if (material.m_diffuseIndex != 255)
									{
										const float diffuseTexelSize = 1.0f / (m_textures[material.m_diffuseIndex]->m_width * m_textures[material.m_diffuseIndex]->m_height);
										if (dUv > diffuseTexelSize * VariableShaderRate || prevMatIndex != tri.m_materialIndex)
										{
											if (material.m_diffuseIndex != -1)
											{
												diffuseSample = m_textures[material.m_diffuseIndex]->Sample<u8vec4>(uv);
												prevUV = uv;
												prevMatIndex = tri.m_materialIndex;
											}
										}

										output[index] = diffuseSample;
									}

									SAILOR_PROFILE_END_BLOCK();
								}
								else
								{
									output[index] = u8vec3(0u, 0u, 0u);
								}

								SAILOR_PROFILE_END_BLOCK();
							}
						}

					}, Tasks::EThreadType::Worker);

				if (((x + y) / GroupSize) % 32 == 0)
				{
					tasksThisThread.Emplace(task);
				}
				else
				{
					task->Run();
					tasks.Emplace(std::move(task));
				}
			}
		}
		SAILOR_PROFILE_END_BLOCK();

		SAILOR_PROFILE_BLOCK("Calcs on Main thread");
		for (auto& task : tasksThisThread)
		{
			task->Execute();
		}
		SAILOR_PROFILE_END_BLOCK();

		SAILOR_PROFILE_BLOCK("Wait all calcs");
		for (auto& task : tasks)
		{
			task->Wait();
		}
		SAILOR_PROFILE_END_BLOCK();
	}

	raytracingTimer.Stop();
	//profiler::dumpBlocksToFile("test_profile.prof");

	char msg[2048];
	sprintf_s(msg, "Time in sec: %.2f", (float)raytracingTimer.ResultMs() * 0.001f);
	MessageBoxA(0, msg, msg, 0);

	SAILOR_PROFILE_BLOCK("Write Image");
	Internal::WriteImage(outputFile, output, width, height);
	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Open Image");
	::system(outputFile);
	SAILOR_PROFILE_END_BLOCK();
}
