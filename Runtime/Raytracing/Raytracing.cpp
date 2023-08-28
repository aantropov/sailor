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
#include "BVH.h"

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

void Raytracing::Run()
{
	SAILOR_PROFILE_FUNCTION();

	Utils::Timer raytracingTimer;
	raytracingTimer.Start();

	const uint32_t GroupSize = 32;

	const char* outputFile = "output.png";

	//const std::filesystem::path sceneFile = "../Content/Models/Sponza2/Sponza.gltf";
	const std::filesystem::path sceneFile = "../Content/Models/ABeautifulGame/ABeautifulGame.gltf";
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

	float aspectRatio = scene->HasCameras() ? scene->mCameras[0]->mAspect : 16.0f / 10.0f;
	const uint32_t width = 1920;
	const uint32_t height = static_cast<uint32_t>(width / aspectRatio);
	const float hFov = scene->HasCameras() ? scene->mCameras[0]->mHorizontalFOV : glm::radians(80.0f);
	const float vFov = 2.0f * atan(tan(hFov / 2.0f) * (1.0f / aspectRatio));

	const float zMin = scene->HasCameras() ? scene->mCameras[0]->mClipPlaneNear : 0.01f;
	const float zMax = scene->HasCameras() ? scene->mCameras[0]->mClipPlaneFar : 1000.0f;

	// Camera
	const mat4 projectionMatrix = transpose(glm::perspectiveFovRH(vFov, (float)width, (float)height, zMin, zMax));
	mat4 viewMatrix{ 1 };

	auto cameraPos = vec3(-1.2f, 0.7f, -1.0f) * 0.5f;
	auto cameraUp = normalize(vec3(0, 1, 0));
	auto cameraForward = normalize(-cameraPos);
	//auto cameraForward = -vec3(0.5f, 0.7f, 1.0f);

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
		auto FillData = [](const aiMaterialProperty* property, const std::string& name, void* ptr)
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
		m_textures.Resize(scene->mNumMaterials * 5);

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
					if (FillData(property, std::string("diffuse"), &material.m_baseColorFactor) ||						
						FillData(property, std::string("emission"), &material.m_emissiveFactor) ||
						FillData(property, std::string("specular"), &material.m_specularColorFactor))
					{
						continue;
					}
				}
			}

			if (!diffuseMaps.IsEmpty())
			{
				Tasks::ITaskPtr task = Tasks::CreateTask("Load Texture", [=]() { LoadTexture(textureIndex, diffuseMaps[0].c_str()); })->Run();
				loadTexturesTasks.Emplace(std::move(task));
				material.m_baseColorIndex = textureIndex;
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
				material.m_emissiveIndex = textureIndex;
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
						ray.SetOrigin(cameraPos);

						for (uint32_t v = 0; v < GroupSize && (y + v) < height; v++)
						{
							float tv = (y + v) / (float)height;
							for (uint32_t u = 0; u < GroupSize && (u + x) < width; u++)
							{
								SAILOR_PROFILE_BLOCK("Raycasting");

								const uint32_t index = (y + v) * width + (x + u);
								float tu = (x + u) / (float)width;

								const vec3 midTop = viewportUpperLeft + (viewportUpperRight - viewportUpperLeft) * tu;
								const vec3 midBottom = viewportBottomLeft + (viewportBottomRight - viewportBottomLeft) * tu;
								ray.SetDirection(glm::normalize(midTop + (midBottom - midTop) * tv - cameraPos));

								output[index] = u8vec3(Raytrace(ray, bvh));
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
	if (raytracingTimer.ResultMs() > 6000.0f)
	{
		sprintf_s(msg, "Time in sec: %.2f", (float)raytracingTimer.ResultMs() * 0.001f);
		MessageBoxA(0, msg, msg, 0);
	}

	SAILOR_PROFILE_BLOCK("Write Image");
	{
		const uint32_t Channels = 3;
		if (!stbi_write_png(outputFile, width, height, Channels, &output[0], width * Channels))
		{
			SAILOR_LOG("Raytracing WriteImage error");
		}
	}
	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Open Image");
	::system(outputFile);
	SAILOR_PROFILE_END_BLOCK();
}


vec3 Raytracing::Raytrace(const Math::Ray& ray, const BVH& bvh) const
{
	SAILOR_PROFILE_FUNCTION();

	RaycastHit hit;
	if (bvh.IntersectBVH(ray, hit, 0))
	{
		SAILOR_PROFILE_BLOCK("Sampling");

		const Math::Triangle& tri = m_triangles[hit.m_triangleIndex];

		const vec3 faceNormal = vec3(hit.m_barycentricCoordinate.x * tri.m_normals[0] + hit.m_barycentricCoordinate.y * tri.m_normals[1] + hit.m_barycentricCoordinate.z * tri.m_normals[2]);
		const vec3 tangent = vec3(hit.m_barycentricCoordinate.x * tri.m_tangent[0] + hit.m_barycentricCoordinate.y * tri.m_tangent[1] + hit.m_barycentricCoordinate.z * tri.m_tangent[2]);
		const vec3 bitangent = vec3(hit.m_barycentricCoordinate.x * tri.m_bitangent[0] + hit.m_barycentricCoordinate.y * tri.m_bitangent[1] + hit.m_barycentricCoordinate.z * tri.m_bitangent[2]);

		const mat3 tbn(tangent, bitangent, faceNormal);

		vec2 uv = hit.m_barycentricCoordinate.x * tri.m_uvs[0] +
			hit.m_barycentricCoordinate.y * tri.m_uvs[1] +
			hit.m_barycentricCoordinate.z * tri.m_uvs[2];

		const auto& material = m_materials[tri.m_materialIndex];
		if (material.m_baseColorIndex != 255)
		{
			//diffuse = m_textures[material.m_baseColorIndex]->Sample<u8vec4>(uv);
		}

		if (material.m_normalIndex != 255)
		{
			//faceNormal = (vec4(m_textures[material.m_normalIndex]->Sample<u8vec4>(uv)) - 128.0f) / 128.0f;
		}

		SAILOR_PROFILE_END_BLOCK();

		return faceNormal;
	}
	return vec3(1);
}