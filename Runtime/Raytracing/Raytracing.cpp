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

void Raytracing::ParseParamsFromCommandLineArgs(Raytracing::Params& res, const char** args, int32_t num)
{
	for (int32_t i = 1; i < num; i += 2)
	{
		if (strcmp("--in", args[i]) == 0)
		{
			res.m_pathToModel = std::string(args[i + 1]);
		}
		else if (strcmp("--out", args[i]) == 0)
		{
			res.m_output = std::string(args[i + 1]);
		}
		else if (strcmp("--height", args[i]) == 0)
		{
			res.m_height = atoi(args[i + 1]);
		}
	}
}

void GenerateTangentBitangent(
	vec3& outTangent,
	vec3& outBitangent,
	const vec3* vert,
	const vec2* uv)
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
		else
		{
			vec3 t = vec3(0.0f, 0.0f, 0.0f);
			vec3 b = vec3(0.0f, 0.0f, 0.0f);
			GenerateTangentBitangent(t, b, &tri.m_vertices[0], &tri.m_uvs[0]);

			memcpy(&tri.m_tangent[0], &t, sizeof(float) * 3);
			memcpy(&tri.m_tangent[1], &t, sizeof(float) * 3);
			memcpy(&tri.m_tangent[2], &t, sizeof(float) * 3);

			memcpy(&tri.m_bitangent[0], &b, sizeof(float) * 3);
			memcpy(&tri.m_bitangent[1], &b, sizeof(float) * 3);
			memcpy(&tri.m_bitangent[2], &b, sizeof(float) * 3);
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

template<typename T>
Tasks::ITaskPtr LoadTexture_Task(TVector<TSharedPtr<Raytracing::Texture2D>>& m_textures,
	const std::filesystem::path& sceneFile,
	const aiScene* scene,
	uint32_t textureIndex,
	const std::string& filename,
	bool bConvertToLinear,
	bool bNormalMap = false)
{
	auto ptr = m_textures[textureIndex] = TSharedPtr<Raytracing::Texture2D>::Make();

	Tasks::ITaskPtr task = Tasks::CreateTask("Load Texture",
		[
			scene = scene,
				pTexture = ptr,
				sceneFile = sceneFile,
				fileName = filename,
				bConvertToLinear = bConvertToLinear,
				bNormalMap = bNormalMap
		]() mutable
		{
			int32_t texChannels = 0;
			void* pixels = nullptr;

			if (fileName[0] == '*')
			{
				const uint32 texIndex = atoi(&fileName[1]);
				aiTexture* pAITexture = scene->mTextures[texIndex];
				pixels = stbi_load_from_memory((stbi_uc*)pAITexture->pcData, pAITexture->mWidth,
					&pTexture->m_width,
					&pTexture->m_height,
					&texChannels, STBI_rgb_alpha);

				pTexture->Initialize<T, u8vec4>((u8vec4*)pixels, bConvertToLinear, false, bNormalMap);
			}
			else
			{
				sceneFile.replace_filename(fileName);
				if (stbi_is_hdr(sceneFile.string().c_str()))
				{
					pixels = stbi_loadf(sceneFile.string().c_str(),
						&pTexture->m_width,
						&pTexture->m_height,
						&texChannels, STBI_rgb_alpha);

					pTexture->Initialize<T, vec4>((vec4*)pixels, bConvertToLinear, false, bNormalMap);
				}
				else
				{
					pixels = stbi_load(sceneFile.string().c_str(),
						&pTexture->m_width,
						&pTexture->m_height,
						&texChannels, STBI_rgb_alpha);

					pTexture->Initialize<T, u8vec4>((u8vec4*)pixels, bConvertToLinear, false, bNormalMap);
				}
			}

			if (pixels)
			{
				stbi_image_free(pixels);
			}
		})->Run();

		return task;
};

void Raytracing::Run(const Raytracing::Params& params)
{
	SAILOR_PROFILE_FUNCTION();

	//EASY_PROFILER_ENABLE

	Utils::Timer raytracingTimer;
	raytracingTimer.Start();

	const uint32_t GroupSize = 32;

	Assimp::Importer importer;

	const unsigned int DefaultImportFlags_Assimp =
		//aiProcess_CalcTangentSpace |
		//aiProcess_Triangulate |
		aiProcess_FlipUVs |
		//aiProcess_SortByPType |
		aiProcess_PreTransformVertices |
		aiProcess_GenNormals |
		aiProcess_GenUVCoords |
		//aiProcess_Debone |
		//aiProcess_FixInfacingNormals |
		//aiProcess_ValidateDataStructure |
		//aiProcess_FindDegenerates |
		//aiProcess_ImproveCacheLocality |
		//aiProcess_FindInvalidData |
		//aiProcess_FlipWindingOrder |
		0;

	const auto scene = importer.ReadFile(params.m_pathToModel.string().c_str(), DefaultImportFlags_Assimp);

	if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
	{
		SAILOR_LOG("%s", importer.GetErrorString());
		return;
	}

	ensure(scene->HasCameras(), "Scene %s has no Cameras!", params.m_pathToModel.string().c_str());

	float aspectRatio = scene->HasCameras() ? scene->mCameras[0]->mAspect : (4.0f / 3.0f);
	const uint32_t height = params.m_height;
	const uint32_t width = static_cast<uint32_t>(height * aspectRatio);
	const float hFov = scene->HasCameras() ? scene->mCameras[0]->mHorizontalFOV : glm::radians(60.0f);
	const float vFov = 2.0f * atan(tan(hFov / 2.0f) * (1.0f / aspectRatio));

	const float zMin = scene->HasCameras() ? scene->mCameras[0]->mClipPlaneNear : 0.01f;
	const float zMax = scene->HasCameras() ? scene->mCameras[0]->mClipPlaneFar : 1000.0f;

	// Camera
	const mat4 projectionMatrix = transpose(glm::perspectiveFovRH(vFov, (float)width, (float)height, zMin, zMax));
	mat4 viewMatrix{ 1 };

	//auto cameraPos = vec3(0.0f, 0.5f, 0.65f) * 0.5f; //vec3(0, 0, 4);
	auto cameraPos = vec3(-1.0f, 0.7f, -1.0f) * 0.5f;
	auto cameraUp = normalize(vec3(0, 1, 0));
	auto cameraForward = normalize(-cameraPos);
	//auto cameraForward = -vec3(0.5f, 0.7f, 1.0f);

	auto axis = normalize(cross(cameraForward, cameraUp));
	cameraUp = normalize(cross(axis, cameraForward));

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
		TVector<Tasks::ITaskPtr> loadTexturesTasks;
		m_materials.Resize(scene->mNumMaterials);
		m_textures.Resize(scene->mNumMaterials * 5);

		uint32_t textureIndex = 0;
		for (uint32_t i = 0; i < scene->mNumMaterials; i++)
		{
			auto& material = m_materials[i];
			const auto& aiMaterial = scene->mMaterials[i];

			aiString fileName;
			if (aiMaterial->GetTexture(AI_MATKEY_BASE_COLOR_TEXTURE, &fileName) == AI_SUCCESS ||
				aiMaterial->GetTexture(aiTextureType_DIFFUSE, 0, &fileName) == AI_SUCCESS)
			{
				const std::string file(fileName.C_Str());
				if (m_textureMapping.ContainsKey(file))
				{
					material.m_baseColorIndex = m_textureMapping[file];
				}
				else
				{
					loadTexturesTasks.Emplace(LoadTexture_Task<vec4>(m_textures, params.m_pathToModel, scene, textureIndex, file, true));
					material.m_baseColorIndex = textureIndex;
					m_textureMapping[file] = textureIndex;
					textureIndex++;
				}
			}

			if (aiMaterial->GetTexture(aiTextureType_NORMALS, 0, &fileName) == AI_SUCCESS)
			{
				const std::string file(fileName.C_Str());
				if (m_textureMapping.ContainsKey(file))
				{
					material.m_normalIndex = m_textureMapping[file];
				}
				else
				{
					loadTexturesTasks.Emplace(LoadTexture_Task<vec3>(m_textures, params.m_pathToModel, scene, textureIndex, file, false, true));
					material.m_normalIndex = textureIndex;
					m_textureMapping[file] = textureIndex;
					textureIndex++;
				}
			}

			if (aiMaterial->GetTexture(AI_MATKEY_METALLIC_TEXTURE, &fileName) == AI_SUCCESS ||
				aiMaterial->GetTexture(AI_MATKEY_ROUGHNESS_TEXTURE, &fileName) == AI_SUCCESS)
			{
				const std::string file(fileName.C_Str());
				if (m_textureMapping.ContainsKey(file))
				{
					material.m_metallicRoughnessIndex = m_textureMapping[file];
				}
				else
				{
					loadTexturesTasks.Emplace(LoadTexture_Task<vec3>(m_textures, params.m_pathToModel, scene, textureIndex, file, false));
					material.m_metallicRoughnessIndex = textureIndex;
					m_textureMapping[file] = textureIndex;
					textureIndex++;
				}
			}

			if (aiMaterial->GetTexture(aiTextureType_EMISSIVE, 0, &fileName) == AI_SUCCESS)
			{
				const std::string file(fileName.C_Str());
				if (m_textureMapping.ContainsKey(file))
				{
					material.m_emissiveIndex = m_textureMapping[file];
				}
				else
				{
					loadTexturesTasks.Emplace(LoadTexture_Task<vec3>(m_textures, params.m_pathToModel, scene, textureIndex, file, true));
					material.m_emissiveIndex = textureIndex;
					m_textureMapping[file] = textureIndex;
					textureIndex++;
				}
			}

			aiColor3D color3D{};
			aiColor4D color4D{};
			float color1D = 0.0f;

			if (aiMaterial->Get(AI_MATKEY_COLOR_EMISSIVE, color3D) == AI_SUCCESS)
			{
				material.m_emissiveFactor = vec3(color3D.r, color3D.g, color3D.b);
			}

			material.m_emissiveFactor *= aiMaterial->Get(AI_MATKEY_EMISSIVE_INTENSITY, color1D) == AI_SUCCESS ? color1D : 1.0f;
			material.m_transmissionFactor = aiMaterial->Get(AI_MATKEY_TRANSMISSION_FACTOR, color1D) == AI_SUCCESS ? color1D : 0.0f;
			material.m_baseColorFactor = (aiMaterial->Get(AI_MATKEY_BASE_COLOR, color4D) == AI_SUCCESS) ? vec4(color4D.r, color4D.g, color4D.b, color4D.a) : glm::vec4(1.0f);
			material.m_roughnessFactor = aiMaterial->Get(AI_MATKEY_ROUGHNESS_FACTOR, color1D) == AI_SUCCESS ? color1D : 1.0f;
			material.m_metallicFactor = aiMaterial->Get(AI_MATKEY_METALLIC_FACTOR, color1D) == AI_SUCCESS ? color1D : 1.0f;
		}

		for (auto& task : loadTexturesTasks)
		{
			task->Wait();
		}
	}

	BVH bvh(numFaces);
	bvh.BuildBVH(m_triangles);

	SAILOR_PROFILE_BLOCK("Viewport Calcs");

	TVector<vec3> output(width * height);

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

								if ((x + u) == 340 && (y + v) == 567)
								{
									volatile uint32_t a = 0;
								}

								vec3 accumulator = vec3(0);
								for (uint32_t sample = 0; sample < params.m_numSamples; sample++)
								{
									auto offset = sample == 0 ? vec2(0, 0) : glm::linearRand(vec2(0, 0), vec2(1.0f / width, 1.0f / height));

									const vec3 midTop = viewportUpperLeft + (viewportUpperRight - viewportUpperLeft) * (tu + offset.x);
									const vec3 midBottom = viewportBottomLeft + (viewportBottomRight - viewportBottomLeft) * (tu + offset.x);
									ray.SetDirection(vec3(0.001f, 0.001f, 0.001f) + glm::normalize(midTop + (midBottom - midTop) * (tv + offset.y) - cameraPos));

									accumulator += Raytrace(ray, bvh, params.m_numBounces, (uint32_t)(-1));
								}

								output[index] = accumulator / (float)params.m_numSamples;
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
	if (raytracingTimer.ResultMs() > 10000.0f)
	{
		sprintf_s(msg, "Time in sec: %.2f", (float)raytracingTimer.ResultMs() * 0.001f);
		MessageBoxA(0, msg, msg, 0);
	}

	SAILOR_PROFILE_BLOCK("Write Image");
	{
		TVector<u8vec3> outSrgb(width * height);
		for (uint32_t i = 0; i < width * height; i++)
		{
			outSrgb[i] = glm::clamp(Utils::LinearToSRGB(output[i]) * 255.0f, 0.0f, 255.0f);
		}

		const uint32_t Channels = 3;
		if (!stbi_write_png(params.m_output.string().c_str(), width, height, Channels, &outSrgb[0], width * Channels))
		{
			SAILOR_LOG("Raytracing WriteImage error");
		}
	}
	SAILOR_PROFILE_END_BLOCK();

	::system(params.m_output.string().c_str());
}

float DistributionGGX(const vec3& N, const vec3& H, float roughness)
{
	float a = roughness * roughness;
	float a2 = a * a;
	float NdotH = std::max(dot(N, H), 0.0001f);
	float NdotH2 = NdotH * NdotH;

	float nom = a2;
	float denom = (NdotH2 * (a2 - 1.0f) + 1.0f);
	denom = glm::pi<float>() * denom * denom;

	return nom / denom;
}

vec3 FresnelSchlick(float cosTheta, const vec3& F0)
{
	return F0 + (1.0f - F0) * glm::pow(1.0f - cosTheta, 5.0f);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
	float k = (roughness * roughness) / 2.0f; // Different k value calculation
	float denom = NdotV * (1.0f - k) + k;
	return NdotV / denom;
}

vec3 Raytracing::CalculatePBR(const vec3& viewDirection, const vec3& worldNormal, const vec3& lightDirection, const SampledData& sample) const
{
	const float ambientOcclusion = sample.m_orm.x;
	const float roughness = sample.m_orm.y;
	const float metallic = sample.m_orm.z;

	const float nDotL = glm::dot(worldNormal, lightDirection);
	const float nDotV = glm::dot(worldNormal, viewDirection);

	vec3 F0 = mix(vec3(0.04f), vec3(sample.m_baseColor), metallic);

	vec3 halfwayVector = normalize(viewDirection + lightDirection);
	float NDF = DistributionGGX(worldNormal, halfwayVector, roughness);
	vec3 F = FresnelSchlick(std::max(glm::dot(halfwayVector, viewDirection), 0.0f), F0);
	float geomA = GeometrySchlickGGX(nDotL, roughness);
	float geomB = GeometrySchlickGGX(nDotV, roughness);
	float geometricTerm = geomA * geomB;

	vec3 kS = F;
	vec3 kD = vec3(1.0f) - kS;
	kD *= 1.0f - metallic;

	// TODO: Two sided
	if (nDotL < 0.0f  || nDotV < 0.0f)
	{
		return vec3(0.0f);
	}

	float denominator = (4.0f * std::max(nDotV, 0.0f) * std::max(nDotL, 0.0f)) + 0.001f;
	vec3 specularTerm = (F * NDF * geometricTerm) / denominator;
	vec3 diffuseTerm = (kD * sample.m_baseColor.xyz) / glm::pi<float>();

	vec3 lighting = (diffuseTerm + specularTerm);// *(vec3(1.0f) - vec3(ambientOcclusion));

	return lighting;
}

vec3 ImportanceSampleGGX(vec2 Xi, float roughness, const vec3& n)
{
	float a = roughness * roughness;

	float phi = 2.0f * glm::pi<float>() * Xi.x;
	float cosTheta = sqrt((1.0f - Xi.y) / (1.0f + (a * a - 1.0f) * Xi.y));
	float sinTheta = sqrt(1.0f - cosTheta * cosTheta);

	vec3 H;
	H.x = sinTheta * cos(phi);
	H.y = sinTheta * sin(phi);
	H.z = cosTheta;

	vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(up, n));
	vec3 bitangent = cross(n, tangent);

	vec3 sampleVec = tangent * H.x + bitangent * H.y + n * H.z;
	return normalize(sampleVec);
}

float GGX_PDF(vec3 N, vec3 H, vec3 V, float roughness)
{
	float a = roughness * roughness;
	float NdotH = std::max(dot(N, H), 0.0001f);
	float VdotH = std::max(dot(V, H), 0.0001f);

	//float D = a / (glm::pi<float>() * pow(NdotH * NdotH * (a - 1.0f) + 1.0f, 2.0f));
	float D = (a * a) / (glm::pi<float>() * pow(NdotH * NdotH * (a * a - 1.0f) + 1.0f, 2.0f));
	float pdf = D * NdotH / (4.0f * VdotH);

	return pdf;
}

vec3 Raytracing::Raytrace(const Math::Ray& ray, const BVH& bvh, uint32_t bounceLimit, uint32_t ignoreTriangle) const
{
	SAILOR_PROFILE_FUNCTION();

	vec3 res = vec3(0);
	RaycastHit hit;
	if (bvh.IntersectBVH(ray, hit, 0, std::numeric_limits<float>().max(), ignoreTriangle))
	{
		SAILOR_PROFILE_BLOCK("Sampling");

		const Math::Triangle& tri = m_triangles[hit.m_triangleIndex];

		const vec3 faceNormal = vec3(hit.m_barycentricCoordinate.x * tri.m_normals[0] + hit.m_barycentricCoordinate.y * tri.m_normals[1] + hit.m_barycentricCoordinate.z * tri.m_normals[2]);
		const vec3 tangent = vec3(hit.m_barycentricCoordinate.x * tri.m_tangent[0] + hit.m_barycentricCoordinate.y * tri.m_tangent[1] + hit.m_barycentricCoordinate.z * tri.m_tangent[2]);
		const vec3 bitangent = vec3(hit.m_barycentricCoordinate.x * tri.m_bitangent[0] + hit.m_barycentricCoordinate.y * tri.m_bitangent[1] + hit.m_barycentricCoordinate.z * tri.m_bitangent[2]);

		const mat3 tbn(tangent, bitangent, faceNormal);

		const vec2 uv = hit.m_barycentricCoordinate.x * tri.m_uvs[0] +
			hit.m_barycentricCoordinate.y * tri.m_uvs[1] +
			hit.m_barycentricCoordinate.z * tri.m_uvs[2];

		const SampledData sample = GetSampledData(tri.m_materialIndex, uv);
		const vec3 viewDirection = -normalize(ray.GetDirection());
		const vec3 worldNormal = normalize(tbn * sample.m_normal);

		RaycastHit hitLight{};
		const vec3 toLight = normalize(vec3(1, 1, -1));
		const vec3 offset = 0.00001f * faceNormal;

		SAILOR_PROFILE_END_BLOCK();

		SAILOR_PROFILE_BLOCK("Direct lighting");
		Ray rayToLight(hit.m_point + offset, toLight);
		if (!bvh.IntersectBVH(rayToLight, hitLight, 0, std::numeric_limits<float>().max(), hit.m_triangleIndex))
		{
			res += CalculatePBR(viewDirection, worldNormal, toLight, sample);
		}

		SAILOR_PROFILE_END_BLOCK();

		SAILOR_PROFILE_BLOCK("Bounces");

		if (bounceLimit > 0)
		{
			const uint32_t numExtraSamples = 64;

			vec3 indirect = vec3(0.0f, 0.0f, 0.0f);
			for (uint32_t i = 0; i < numExtraSamples; i++)
			{
				vec2 randomSample = vec2(glm::linearRand(0.0f, 1.0f), glm::linearRand(0.0f, 1.0f));
				vec3 H = ImportanceSampleGGX(randomSample, sample.m_orm.y, worldNormal);
				vec3 direction = 2.0f * dot(viewDirection, H) * H - viewDirection;

				float pdf = GGX_PDF(worldNormal, H, viewDirection, sample.m_orm.y);
				float weight = std::min(50.0f, 1.0f / pdf);

				const Ray rayToDirection(hit.m_point + offset, direction);
				indirect += weight * CalculatePBR(viewDirection, worldNormal, direction, sample) * Raytrace(rayToDirection, bvh, bounceLimit - 1, hit.m_triangleIndex);
			}
			res += indirect / (float)numExtraSamples;
		}

		res += sample.m_emissive;

		SAILOR_PROFILE_END_BLOCK();
	}
	else
	{
		res = vec3(0.5f, 0.5f, 0.5f);
	}

	return res;
}

Raytracing::SampledData Raytracing::GetSampledData(const size_t& materialIndex, glm::vec2 uv) const
{
	const auto& material = m_materials[materialIndex];

	Raytracing::SampledData res{};
	res.m_baseColor = material.m_baseColorFactor;
	res.m_normal = vec3(0, 0, 1.0f);
	res.m_orm = vec3(0.0f, material.m_roughnessFactor, material.m_metallicFactor);
	res.m_emissive = material.m_emissiveFactor;

	if (material.HasBaseTexture())
	{
		res.m_baseColor *= m_textures[material.m_baseColorIndex]->Sample<vec4>(uv);
	}

	if (material.HasEmissiveTexture())
	{
		res.m_emissive *= m_textures[material.m_emissiveIndex]->Sample<vec3>(uv);
	}

	if (material.HasMetallicRoughnessTexture())
	{
		const vec3 ormSample = m_textures[material.m_metallicRoughnessIndex]->Sample<vec3>(uv);
		res.m_orm = vec3(ormSample.r, res.m_orm.g * ormSample.g, res.m_orm.b * ormSample.b);
	}

	if (material.HasNormalTexture())
	{
		res.m_normal = m_textures[material.m_normalIndex]->Sample<vec3>(uv);
	}

	return res;
}