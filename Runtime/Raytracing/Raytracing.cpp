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

bool BVH::IntersectBVH(const Math::Ray& ray, const TVector<Math::Triangle>& tris, Math::RaycastHit& outResult, const uint nodeIdx, float maxRayLength) const
{
	check(tris.Num() * 2 - 1 == m_nodes.Num());

	const BVHNode& node = m_nodes[nodeIdx];
	if (!Math::IntersectRayAABB(ray, node.m_aabbMin, node.m_aabbMax))
	{
		return false;
	}

	if (node.IsLeaf())
	{
		RaycastHit res{};
		for (uint i = 0; i < node.m_triCount; i++)
		{
			Math::IntersectRayTriangle(ray, tris[m_triIdx[node.m_firstTriIdx + i]], res, outResult.m_rayLenght);
			if (res.m_rayLenght < outResult.m_rayLenght)
			{
				outResult = res;
				outResult.m_triangleIndex += node.m_firstTriIdx + i;
			}
		}
	}
	else
	{
		RaycastHit res1{};
		IntersectBVH(ray, tris, res1, node.m_leftNode, res1.m_rayLenght);

		RaycastHit res2{};
		IntersectBVH(ray, tris, res2, node.m_leftNode + 1, res2.m_rayLenght);

		outResult = res1.m_rayLenght < res2.m_rayLenght ? res1 : res2;
	}

	return outResult.HasIntersection();
}

void BVH::UpdateNodeBounds(uint32_t nodeIdx, const TVector<Math::Triangle>& tris)
{
	BVH::BVHNode& node = m_nodes[nodeIdx];

	node.m_aabbMin = vec3(1e30f);
	node.m_aabbMax = vec3(-1e30f);

	for (uint first = node.m_firstTriIdx, i = 0; i < node.m_triCount; i++)
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
	// terminate recursion
	BVH::BVHNode& node = m_nodes[nodeIdx];

	if (node.m_triCount <= 2)
	{
		return;
	}

	// determine split axis and position
	vec3 extent = node.m_aabbMax - node.m_aabbMin;
	int32_t axis = 0;

	if (extent.y > extent.x)
	{
		axis = 1;
	}

	if (extent.z > extent[axis])
	{
		axis = 2;
	}

	float splitPos = node.m_aabbMin[axis] + extent[axis] * 0.5f;

	// in-place partition
	int32_t i = node.m_firstTriIdx;
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
	uint32_t leftCount = i - node.m_firstTriIdx;

	if (leftCount == 0 || leftCount == node.m_triCount)
	{
		return;
	}

	// create child nodes
	uint32_t leftChildIdx = m_nodesUsed++;
	uint32_t rightChildIdx = m_nodesUsed++;

	m_nodes[leftChildIdx].m_firstTriIdx = node.m_firstTriIdx;
	m_nodes[leftChildIdx].m_triCount = leftCount;
	m_nodes[rightChildIdx].m_firstTriIdx = i;
	m_nodes[rightChildIdx].m_triCount = node.m_triCount - leftCount;

	node.m_leftNode = leftChildIdx;
	node.m_triCount = 0;

	UpdateNodeBounds(leftChildIdx, tris);
	UpdateNodeBounds(rightChildIdx, tris);

	Subdivide(leftChildIdx, tris);
	Subdivide(rightChildIdx, tris);
}

void BVH::BuildBVH(const TVector<Math::Triangle>& tris)
{
	check(tris.Num() * 2 - 1 == m_nodes.Num());

	for (uint32_t i = 0; i < m_nodes.Num(); i++)
	{
		m_triIdx[i] = i;
	}

	BVHNode& root = m_nodes[m_rootNodeIdx];
	root.m_leftNode = 0;
	root.m_firstTriIdx = 0;
	root.m_triCount = (uint32_t)tris.Num();

	UpdateNodeBounds(m_rootNodeIdx, tris);
	Subdivide(m_rootNodeIdx, tris);
}

namespace Sailor::Internal
{
	void WriteImage(const char* outputFileName, const TVector<u8vec3>& data, uint32_t width, uint32_t height)
	{
		const uint32_t Channels = 3;

		if (!stbi_write_png(outputFileName, width, height, Channels, &data[0], width * Channels))
		{
			SAILOR_LOG("Raytracing WriteImage error");
		}
	}
}

Tasks::TaskPtr<TSharedPtr<Raytracing::Texture2D<u8vec4>>> Raytracing::LoadTexture(const char* file)
{
	auto task = Tasks::CreateTaskWithResult<TSharedPtr<Raytracing::Texture2D<u8vec4>>>(file,
		[file = file]() mutable
		{
			TSharedPtr<Raytracing::Texture2D<u8vec4>> tex = TSharedPtr<Raytracing::Texture2D<u8vec4>>::Make();

			int32_t width{};
			int32_t height{};
			uint32_t imageSize{};
			void* pixels = nullptr;

			int32_t texChannels = 0;
			if (stbi_is_hdr(file) && (pixels = stbi_loadf(file, &width, &height, &texChannels, STBI_rgb_alpha)))
			{
				imageSize = (uint32_t)width * height * sizeof(float) * 4;
			}
			else if (pixels = stbi_load(file, &width, &height, &texChannels, STBI_rgb_alpha))
			{
				imageSize = (uint32_t)width * height * 4;
			}
			else
			{
				check(false);
			}

			tex->m_data.Resize(imageSize);
			::memcpy(tex->m_data.GetData(), pixels, imageSize);
			stbi_image_free(pixels);

			return tex;
		});

	return task;
}

void ProcessMesh_Assimp(aiMesh* mesh, TVector<Triangle>& outScene, const mat4& viewProjection, const aiScene* scene)
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

		auto v0 = vec4(*reinterpret_cast<vec3*>(&mesh->mVertices[face.mIndices[0]]), 1) * viewProjection;
		auto v1 = vec4(*reinterpret_cast<vec3*>(&mesh->mVertices[face.mIndices[1]]), 1) * viewProjection;
		auto v2 = vec4(*reinterpret_cast<vec3*>(&mesh->mVertices[face.mIndices[2]]), 1) * viewProjection;

		tri.m_vertices[0] = vec3(v0.xyz) / v0.w;
		tri.m_vertices[1] = vec3(v1.xyz) / v1.w;
		tri.m_vertices[2] = vec3(v2.xyz) / v2.w;

		tri.m_centroid = (tri.m_vertices[0] + tri.m_vertices[1] + tri.m_vertices[2]) * 0.333f;

		mat3 viewProjectionN = viewProjection;

		auto n0 = *reinterpret_cast<vec3*>(&mesh->mNormals[face.mIndices[0]]) * viewProjectionN;
		auto n1 = *reinterpret_cast<vec3*>(&mesh->mNormals[face.mIndices[1]]) * viewProjectionN;
		auto n2 = *reinterpret_cast<vec3*>(&mesh->mNormals[face.mIndices[2]]) * viewProjectionN;

		tri.m_normals[0] = n0;
		tri.m_normals[1] = n1;
		tri.m_normals[2] = n2;

		if (mesh->HasTextureCoords(0))
		{
			tri.m_uvs[0] =
			{
				mesh->mTextureCoords[0][face.mIndices[0]].x,
				mesh->mTextureCoords[0][face.mIndices[0]].y
			};

			tri.m_uvs[1] =
			{
				mesh->mTextureCoords[0][face.mIndices[1]].x,
				mesh->mTextureCoords[0][face.mIndices[1]].y
			};

			tri.m_uvs[2] =
			{
				mesh->mTextureCoords[0][face.mIndices[2]].x,
				mesh->mTextureCoords[0][face.mIndices[2]].y
			};
		}

		if (mesh->HasTangentsAndBitangents())
		{
			tri.m_tangent[0] =
			{
				mesh->mTangents[face.mIndices[0]].x,
				mesh->mTangents[face.mIndices[0]].y,
				mesh->mTangents[face.mIndices[0]].z
			};

			tri.m_tangent[1] =
			{
				mesh->mTangents[face.mIndices[1]].x,
				mesh->mTangents[face.mIndices[1]].y,
				mesh->mTangents[face.mIndices[1]].z
			};

			tri.m_tangent[2] =
			{
				mesh->mTangents[face.mIndices[2]].x,
				mesh->mTangents[face.mIndices[2]].y,
				mesh->mTangents[face.mIndices[2]].z
			};

			tri.m_bitangent[0] =
			{
				mesh->mBitangents[face.mIndices[0]].x,
				mesh->mBitangents[face.mIndices[0]].y,
				mesh->mBitangents[face.mIndices[0]].z
			};

			tri.m_bitangent[1] =
			{
				mesh->mBitangents[face.mIndices[1]].x,
				mesh->mBitangents[face.mIndices[1]].y,
				mesh->mBitangents[face.mIndices[1]].z
			};

			tri.m_bitangent[2] =
			{
				mesh->mBitangents[face.mIndices[2]].x,
				mesh->mBitangents[face.mIndices[2]].y,
				mesh->mBitangents[face.mIndices[2]].z
			};
		}

		tri.m_materialIndex = mesh->mMaterialIndex;
		//memcpy(&mesh->mVertices[face.mIndices[0]], tri.m_vertices, sizeof(float) * 3);
	}
}

void ProcessNode_Assimp(TVector<Triangle>& outScene, const mat4& viewProjection, aiNode* node, const aiScene* scene)
{
	SAILOR_PROFILE_FUNCTION();

	for (uint32_t i = 0; i < node->mNumMeshes; i++)
	{
		aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
		ProcessMesh_Assimp(mesh, outScene, viewProjection, scene);
	}

	for (uint32_t i = 0; i < node->mNumChildren; i++)
	{
		ProcessNode_Assimp(outScene, viewProjection, node->mChildren[i], scene);
	}
}

void Raytracing::Run()
{
	SAILOR_PROFILE_FUNCTION();

	const uint32_t GroupSize = 8;

	const char* outputFile = "output.png";
	const char* sceneFile = "../Content/Models/Duck/Duck.gltf";

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
		aiProcess_ValidateDataStructure |
		aiProcess_FindDegenerates |
		aiProcess_ImproveCacheLocality |
		aiProcess_FindInvalidData |
		//aiProcess_MakeLeftHanded |
		aiProcess_FlipWindingOrder;

	const auto scene = importer.ReadFile(sceneFile, DefaultImportFlags_Assimp);

	if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
	{
		SAILOR_LOG("%s", importer.GetErrorString());
		return;
	}

	ensure(scene->HasCameras(), "Scene %s has no Cameras!", sceneFile);

	float aspectRatio = 16.0f / 9.0f;
	const uint32_t width = 320;
	const uint32_t height = static_cast<uint32_t>(width / aspectRatio);

	// Camera
	mat4 projectionMatrix = transpose(glm::perspectiveFovRH(45.0f, (float)width, (float)height, 0.01f, 1000.0f));
	mat4 viewMatrix{ 1 };

	if (scene->HasCameras())
	{
		const auto& aiCamera = scene->mCameras[0];

		// Projection
		aspectRatio = aiCamera->mAspect;
		projectionMatrix = transpose(glm::perspectiveFovRH(aiCamera->mHorizontalFOV, (float)width, (float)height, aiCamera->mClipPlaneNear, aiCamera->mClipPlaneFar));

		// View
		aiNode* rootNode = scene->mRootNode;
		aiNode* cameraNode = rootNode->FindNode(aiCamera->mName);

		mat4 cameraTransformationMatrix{};
		::memcpy(&cameraTransformationMatrix, &cameraNode->mTransformation, sizeof(float) * 16);

		vec4 camPos{ 0,0,0,1 };
		::memcpy(&camPos, &aiCamera->mPosition, sizeof(float) * 3);
		camPos = camPos * cameraTransformationMatrix;
		camPos /= camPos.w;

		vec3 viewDir{};
		::memcpy(&viewDir, &aiCamera->mLookAt, sizeof(float) * 3);
		viewDir = vec3(vec4(viewDir, 0) * cameraTransformationMatrix);
		viewDir = normalize(viewDir);

		vec3 camUp{};
		::memcpy(&camUp, &aiCamera->mUp, sizeof(float) * 3);
		camUp = normalize(camUp);

		viewMatrix = transpose(glm::lookAtRH(vec3(camPos.xyz), vec3(camPos.xyz) + viewDir, camUp));
	}

	uint32_t numFaces = 0;
	for (uint32_t i = 0; i < scene->mNumMeshes; i++)
	{
		numFaces += scene->mMeshes[i]->mNumFaces;
	}
	m_triangles.Reserve(numFaces);

	ProcessNode_Assimp(m_triangles, viewMatrix * projectionMatrix, scene->mRootNode, scene);

	// Loading textures
	{
		TVector<Tasks::ITaskPtr> tasks;
		tasks.Reserve(scene->mNumMaterials * 4);
		for (uint32_t i = 0; i < scene->mNumMaterials; i++)
		{
			auto& material = scene->mMaterials[i];
			aiString path;
			material->GetTexture(aiTextureType_DIFFUSE, 0, &path);

			tasks.Add(LoadTexture(path.C_Str()));
			(*tasks.Last())->Run();
		}

		for (auto& task : tasks)
		{
			task->Wait();
		}
	}

	BVH bvh(numFaces);
	bvh.BuildBVH(m_triangles);

	TVector<u8vec3> output(width * height * sizeof(u8vec3));

	// Basic consts
	auto focalLength = -1.0f;
	auto cameraCenter = vec3(0, 0, 0);

	// Calculate the vectors across the horizontal and down the vertical viewport edges.
	auto viewportU = vec3(2.0f, 0, 0);
	auto viewportV = vec3(0, -2.0f, 0);

	// Calculate the horizontal and vertical delta vectors from pixel to pixel.
	auto pixelDeltaU = viewportU / (float)width;
	auto pixelDeltaV = viewportV / (float)height;

	// Calculate the location of the upper left pixel.
	auto viewportUpperLeft = cameraCenter - vec3(0, 0, focalLength) - viewportU / 2.0f - viewportV / 2.0f;
	auto pixel00Loc = viewportUpperLeft + 0.5f * (pixelDeltaU + pixelDeltaV);

	// Raytracing
	{
		TVector<Tasks::ITaskPtr> tasks;
		tasks.Reserve((height * width) / (GroupSize * GroupSize));
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
						for (uint32_t v = 0; v < GroupSize; v++)
						{
							for (uint32_t u = 0; u < GroupSize; u++)
							{
								RaycastHit hit;
								const uint32_t index = (y + v) * width + (x + u);
								auto pixelCenter = pixel00Loc + ((float)(x + u) * pixelDeltaU) + ((float)(y + v) * pixelDeltaV);

								ray.m_origin = cameraCenter;
								ray.m_direction = glm::normalize(pixelCenter - cameraCenter);

								output[index] = u8vec3(0u, 0u, 255u);

								//if (Math::IntersectRayTriangle(ray, m_triangles, hit) && abs(hit.m_point.z) < 1.0f)
								if (bvh.IntersectBVH(ray, m_triangles, hit, 0) && hit.m_point.z < 1.0f && hit.m_point.z > 0.0f)
								{
									uint8_t depth = (uint8_t)(pow(hit.m_point.z, 30) * 255.0f);
									//output[index] = u8vec3(255u, 255u, depth);

									const Math::Triangle& tri = m_triangles[hit.m_triangleIndex];

									vec3 normal = 255.0f * (normalize(hit.m_barycentricCoordinate.x * tri.m_normals[0] +
										hit.m_barycentricCoordinate.y * tri.m_normals[1] +
										hit.m_barycentricCoordinate.z * tri.m_normals[2]));

									vec2 uv = 255.0f * (normalize(hit.m_barycentricCoordinate.x * tri.m_uvs[0] +
										hit.m_barycentricCoordinate.y * tri.m_uvs[1] +
										hit.m_barycentricCoordinate.z * tri.m_uvs[2]));

									output[index] = vec3(uv.x, uv.y, 1);
								}
							}
						}

					}, Tasks::EThreadType::Worker);

				task->Run();
				tasks.Emplace(std::move(task));
			}
		}

		for (auto& task : tasks)
		{
			task->Wait();
		}
	}

	Internal::WriteImage(outputFile, output, width, height);
	::system(outputFile);
}
