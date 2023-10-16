#include "PathTracer.h"
#include "Tasks/Scheduler.h"
#include "Core/LogMacros.h"
#include "Core/Utils.h"
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

using namespace Sailor;
using namespace Sailor::Math;
using namespace Sailor::Raytracing;

std::string GetArgValue(const char** args, int32_t& i, int32_t num)
{
	if (i + 1 >= num)
	{
		return "";
	}

	i++;
	std::string value = args[i];

	if (value[0] == '\"')
	{
		while (i < num && value[value.length() - 1] != '\"')
		{
			i++;
			value += " " + std::string(args[i]);
		}
		value = value.substr(1, value.length() - 2);
	}

	return value;
}

void PathTracer::ParseParamsFromCommandLineArgs(PathTracer::Params& res, const char** args, int32_t num)
{
	for (int32_t i = 1; i < num; i++)
	{
		std::string arg = args[i];

		if (arg == "--in")
		{
			res.m_pathToModel = GetArgValue(args, i, num);
		}
		else if (arg == "--out")
		{
			res.m_output = GetArgValue(args, i, num);
		}
		else if (arg == "--height")
		{
			res.m_height = atoi(GetArgValue(args, i, num).c_str());
		}
		else if (arg == "--samples")
		{
			const uint32_t samples = atoi(GetArgValue(args, i, num).c_str());

			res.m_msaa = samples <= 32 ? std::min(4u, samples) : 8u;
			res.m_numSamples = std::max(1u, (uint32_t)std::lround(samples / (float)res.m_msaa));
		}
		else if (arg == "--bounces")
		{
			res.m_numBounces = atoi(GetArgValue(args, i, num).c_str());
		}
		else if (arg == "--camera")
		{
			res.m_camera = GetArgValue(args, i, num);
		}
		else if (arg == "--ambient")
		{
			const std::string& hexStr = GetArgValue(args, i, num);
			const int32_t r = std::stoi(hexStr.substr(0, 2), nullptr, 16);
			const int32_t g = std::stoi(hexStr.substr(2, 2), nullptr, 16);
			const int32_t b = std::stoi(hexStr.substr(4, 2), nullptr, 16);

			res.m_ambient = glm::vec3(r / 255.0f, g / 255.0f, b / 255.0f);
		}
	}
}

void PathTracer::Run(const PathTracer::Params& params)
{
	SAILOR_PROFILE_FUNCTION();

	//EASY_PROFILER_ENABLE

	Utils::Timer raytracingTimer;
	raytracingTimer.Start();

	const uint32_t GroupSize = 32;

	Assimp::Importer importer;

	const unsigned int DefaultImportFlags_Assimp =
		aiProcess_FlipUVs |
		aiProcess_GenNormals |
		aiProcess_GenUVCoords |

		//We don't need that since we are generating the missing tangents/bitangents with our own
		//aiProcess_CalcTangentSpace |

		//We only support gltf, which by specification contains only triangles
		//aiProcess_Triangulate |
		//aiProcess_SortByPType |

		// Assimp wrongly applies the matrix transform on camera pos, that's why we apply the matrix calcs with our own
		//aiProcess_PreTransformVertices |

		// We expect only valid gltfs
		//aiProcess_Debone |
		//aiProcess_ValidateDataStructure |
		//aiProcess_FixInfacingNormals |
		//aiProcess_FindInvalidData |
		//aiProcess_FindDegenerates |
		//aiProcess_ImproveCacheLocality |
		//aiProcess_FlipWindingOrder |
		0;

	const auto scene = importer.ReadFile(params.m_pathToModel.string().c_str(), DefaultImportFlags_Assimp);

	if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
	{
		SAILOR_LOG("%s", importer.GetErrorString());
		return;
	}

	ensure(scene->HasCameras(), "Scene %s has no Cameras!", params.m_pathToModel.string().c_str());

	// Camera
	auto cameraPos = vec3(0, 0, 10);
	//auto cameraPos = vec3(-1.0f, 0.7f, -1.0f) * 0.5f;
	//auto cameraPos = glm::vec3(-2.8f, 2.7f, 5.5f) * 100.0f;

	auto cameraUp = normalize(vec3(0, 1, 0));
	auto cameraForward = normalize(-cameraPos);
	//auto cameraForward = glm::normalize(glm::vec3(0.0f, -0.0f, -1.0f));

	auto axis = normalize(cross(cameraForward, cameraUp));
	cameraUp = normalize(cross(axis, cameraForward));

	// View
	int32_t cameraIndex = 0;
	if (scene->HasCameras())
	{
		for (uint32_t i = 0; i < scene->mNumCameras; i++)
		{
			if (std::strcmp(params.m_camera.c_str(), scene->mCameras[i]->mName.C_Str()) == 0)
			{
				cameraIndex = i;
				break;
			}
		}

		const auto& aiCamera = scene->mCameras[cameraIndex];

		mat4 matrix = GetWorldTransformMatrix(scene, scene->mCameras[cameraIndex]->mName.C_Str());
		::memcpy(&cameraUp, &aiCamera->mUp, sizeof(float) * 3);
		::memcpy(&cameraForward, &aiCamera->mLookAt, sizeof(float) * 3);
		::memcpy(&cameraPos, &aiCamera->mPosition, sizeof(float) * 3);

		const vec4 translation = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f) * matrix;
		cameraPos = vec3(translation.xyz) / translation.w;
		cameraUp = glm::normalize(glm::vec3(glm::vec4(cameraUp, 0.0f) * matrix));
		cameraForward = glm::normalize(glm::vec3(glm::vec4(cameraForward, 0.0f) * matrix));
	}

	const float aspectRatio = (scene->HasCameras() && scene->mCameras[cameraIndex]->mAspect > 0.0f)
		? scene->mCameras[cameraIndex]->mAspect
		: (4.0f / 3.0f);

	const uint32_t height = params.m_height;
	const uint32_t width = static_cast<uint32_t>(height * aspectRatio);

	const float hFov = (scene->HasCameras() && scene->mCameras[cameraIndex]->mHorizontalFOV > 0.0f)
		? scene->mCameras[cameraIndex]->mHorizontalFOV
		: glm::radians(90.0f);

	const float vFov = 2.0f * atan(tan(hFov * 0.5f) * (1.0f / aspectRatio));

	const float zMin = scene->HasCameras() ? scene->mCameras[cameraIndex]->mClipPlaneNear : 0.01f;
	const float zMax = scene->HasCameras() ? scene->mCameras[cameraIndex]->mClipPlaneFar : 1000.0f;

	const auto cameraRight = normalize(cross(cameraForward, cameraUp));

	uint32_t expectedNumFaces = 0;
	for (uint32_t i = 0; i < scene->mNumMeshes; i++)
	{
		expectedNumFaces += scene->mMeshes[i]->mNumFaces;
	}

	m_triangles.Reserve(expectedNumFaces);
	ProcessNode_Assimp(m_triangles, scene->mRootNode, scene, glm::mat4(1.0f));

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

			// Parse specific properties
			for (uint32_t j = 0; j < aiMaterial->mNumProperties; j++)
			{
				const auto& prop = aiMaterial->mProperties[j];
				if (strcmp("$mat.gltf.alphaMode", prop->mKey.C_Str()) == 0)
				{
					check(prop->mType == aiPropertyTypeInfo::aiPTI_String);

					material.m_blendMode = BlendMode::Opaque;

					aiString s;
					if (aiReturn::aiReturn_SUCCESS != aiGetMaterialString(aiMaterial, prop->mKey.data, prop->mSemantic, prop->mIndex, &s))
					{
						continue;
					}

					if (strcmp("BLEND", s.C_Str()) == 0)
					{
						material.m_blendMode = BlendMode::Blend;
					}
					else if (strcmp("MASK", s.C_Str()) == 0)
					{
						material.m_blendMode = BlendMode::Mask;
					}
				}
				else if (strcmp("$mat.gltf.alphaCutoff", prop->mKey.C_Str()) == 0)
				{
					check(prop->mType == aiPropertyTypeInfo::aiPTI_Float);
					memcpy(&material.m_alphaCutoff, prop->mData, 4);
				}
			}

			aiTextureMapMode mapping[2] = { aiTextureMapMode::aiTextureMapMode_Wrap };

			if ((aiMaterial->GetTexture(AI_MATKEY_BASE_COLOR_TEXTURE, &fileName, nullptr, nullptr, nullptr, nullptr, mapping) == AI_SUCCESS) ||
				aiMaterial->GetTexture(aiTextureType_DIFFUSE, 0, &fileName, nullptr, nullptr, nullptr, nullptr, mapping) == AI_SUCCESS)
			{
				const std::string file(fileName.C_Str());
				auto clamping = mapping[0] == aiTextureMapMode::aiTextureMapMode_Wrap ? SamplerClamping::Repeat : SamplerClamping::Clamp;
				if (m_textureMapping.ContainsKey(file) &&
					m_textures[m_textureMapping[file]]->m_clamping == clamping &&
					m_textures[m_textureMapping[file]]->m_channels == 4)
				{
					material.m_baseColorIndex = m_textureMapping[file];
				}
				else
				{
					loadTexturesTasks.Emplace(LoadTexture_Task<vec4>(m_textures, params.m_pathToModel, scene, textureIndex, file, mapping[0], true));
					m_textureMapping[file] = material.m_baseColorIndex = textureIndex++;
				}
			}

			mapping[0] = mapping[1] = aiTextureMapMode::aiTextureMapMode_Wrap;

			if (aiMaterial->GetTexture(aiTextureType_NORMALS, 0, &fileName, nullptr, nullptr, nullptr, nullptr, mapping) == AI_SUCCESS)
			{
				const std::string file(fileName.C_Str());
				auto clamping = mapping[0] == aiTextureMapMode::aiTextureMapMode_Wrap ? SamplerClamping::Repeat : SamplerClamping::Clamp;
				if (m_textureMapping.ContainsKey(file) &&
					m_textures[m_textureMapping[file]]->m_clamping == clamping &&
					m_textures[m_textureMapping[file]]->m_channels == 3)
				{
					material.m_normalIndex = m_textureMapping[file];
				}
				else
				{
					loadTexturesTasks.Emplace(LoadTexture_Task<vec3>(m_textures, params.m_pathToModel, scene, textureIndex, file, mapping[0], false, true));
					m_textureMapping[file] = material.m_normalIndex = textureIndex++;
				}
			}

			mapping[0] = mapping[1] = aiTextureMapMode::aiTextureMapMode_Wrap;

			if ((aiMaterial->GetTexture(AI_MATKEY_METALLIC_TEXTURE, &fileName, nullptr, nullptr, nullptr, nullptr, mapping) == AI_SUCCESS) ||
				aiMaterial->GetTexture(AI_MATKEY_ROUGHNESS_TEXTURE, &fileName, nullptr, nullptr, nullptr, nullptr, mapping) == AI_SUCCESS)
			{
				const std::string file(fileName.C_Str());
				auto clamping = mapping[0] == aiTextureMapMode::aiTextureMapMode_Wrap ? SamplerClamping::Repeat : SamplerClamping::Clamp;
				if (m_textureMapping.ContainsKey(file) &&
					m_textures[m_textureMapping[file]]->m_clamping == clamping &&
					m_textures[m_textureMapping[file]]->m_channels == 3)
				{
					material.m_metallicRoughnessIndex = m_textureMapping[file];
				}
				else
				{
					loadTexturesTasks.Emplace(LoadTexture_Task<vec3>(m_textures, params.m_pathToModel, scene, textureIndex, file, mapping[0], false));
					m_textureMapping[file] = material.m_metallicRoughnessIndex = textureIndex++;
				}
			}

			mapping[0] = mapping[1] = aiTextureMapMode::aiTextureMapMode_Wrap;

			if (aiMaterial->GetTexture(aiTextureType_EMISSIVE, 0, &fileName, nullptr, nullptr, nullptr, nullptr, mapping) == AI_SUCCESS)
			{
				const std::string file(fileName.C_Str());
				auto clamping = mapping[0] == aiTextureMapMode::aiTextureMapMode_Wrap ? SamplerClamping::Repeat : SamplerClamping::Clamp;
				if (m_textureMapping.ContainsKey(file) && 
					m_textures[m_textureMapping[file]]->m_clamping == clamping &&
					m_textures[m_textureMapping[file]]->m_channels == 3)
				{
					material.m_emissiveIndex = m_textureMapping[file];
				}
				else
				{
					loadTexturesTasks.Emplace(LoadTexture_Task<vec3>(m_textures, params.m_pathToModel, scene, textureIndex, file, mapping[0], true));
					m_textureMapping[file] = material.m_emissiveIndex = textureIndex++;
				}
			}

			mapping[0] = mapping[1] = aiTextureMapMode::aiTextureMapMode_Wrap;

			if (aiMaterial->GetTexture(aiTextureType_TRANSMISSION, 0, &fileName, nullptr, nullptr, nullptr, nullptr, mapping) == AI_SUCCESS)
			{
				const std::string file(fileName.C_Str());
				if (m_textureMapping.ContainsKey(file))
				{
					material.m_transmissionIndex = m_textureMapping[file];
				}
				else
				{
					loadTexturesTasks.Emplace(LoadTexture_Task<vec3>(m_textures, params.m_pathToModel, scene, textureIndex, file, mapping[0], false));
					m_textureMapping[file] = material.m_transmissionIndex = textureIndex++;
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

	if (scene->HasLights())
	{
		aiNode* pRootNode = scene->mRootNode;

		for (uint32_t i = 0; i < scene->mNumLights; i++)
		{
			const aiNode* pLightNode = pRootNode->FindNode(scene->mLights[i]->mName);
			const mat4 matrix = GetWorldTransformMatrix(scene, scene->mLights[i]->mName.C_Str());

			if (scene->mLights[i]->mType == aiLightSource_DIRECTIONAL)
			{
				m_directionalLights.Add(DirectionalLight());
				memcpy(&m_directionalLights[i].m_direction, &scene->mLights[i]->mDirection, sizeof(float) * 3);
				memcpy(&m_directionalLights[i].m_intensity, &scene->mLights[i]->mColorDiffuse, sizeof(float) * 3);

				m_directionalLights[i].m_direction = glm::normalize(glm::vec3(glm::vec4(m_directionalLights[i].m_direction, 0.0f) * matrix));
				m_directionalLights[i].m_intensity /= 683.0f;
			}
		}
	}
	/*
	else
	{
		DirectionalLight defaultLight;
		defaultLight.m_direction = normalize(vec3(1, 1, -1));
		defaultLight.m_intensity = vec3(3.0f, 3.0f, 3.0f);

		m_directionalLights.Add(defaultLight);
	}*/

	BVH bvh((uint32_t)m_triangles.Num());
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

		std::atomic<uint32_t> finishedTasks = 0;
		const uint32_t numTasks = (height * width) / (GroupSize * GroupSize);

		tasks.Reserve(numTasks);
		tasksThisThread.Reserve(numTasks / 32);

		for (uint32_t y = 0; y < height; y += GroupSize)
		{
			for (uint32_t x = 0; x < width; x += GroupSize)
			{
				auto task = Tasks::CreateTask("Calculate raytracing",
					[=,
					&finishedTasks,
					&output,
					&bvh,
					this]() mutable
					{
						Ray ray;
						ray.SetOrigin(cameraPos);

#ifdef _DEBUG
						uint32_t debugX = 133;
						uint32_t debugY = 276;

						if (!(x < debugX && (x + GroupSize) > debugX &&
							y < debugY && (y + GroupSize) > debugY))
						{
							return;
						}
#endif
						for (uint32_t v = 0; (v < GroupSize) && (y + v) < height; v++)
						{
							float tv = (y + v) / (float)height;
							for (uint32_t u = 0; u < GroupSize && (u + x) < width; u++)
							{
								SAILOR_PROFILE_BLOCK("Raycasting");

								const uint32_t index = (y + v) * width + (x + u);
								float tu = (x + u) / (float)width;

#ifdef _DEBUG
								if ((x + u) == debugX && ((y + v) == debugY))
								{
									volatile uint32_t a = 0;
								}
#endif
								vec3 accumulator = vec3(0);
								for (uint32_t sample = 0; sample < params.m_msaa; sample++)
								{
									auto offset = sample == 0 ? vec2(0, 0) : glm::linearRand(vec2(0, 0), vec2(1.0f / width, 1.0f / height));

									const vec3 midTop = viewportUpperLeft + (viewportUpperRight - viewportUpperLeft) * (tu + offset.x);
									const vec3 midBottom = viewportBottomLeft + (viewportBottomRight - viewportBottomLeft) * (tu + offset.x);
									ray.SetDirection(vec3(0.001f, 0.001f, 0.001f) + glm::normalize(midTop + (midBottom - midTop) * (tv + offset.y) - cameraPos));

									accumulator += Raytrace(ray, bvh, params.m_numBounces, (uint32_t)(-1), params);
								}

								output[index] = accumulator / (float)params.m_msaa;
								SAILOR_PROFILE_END_BLOCK();
							}
						}

						finishedTasks++;

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
		float lastPrg = 0.0f;
		for (auto& task : tasksThisThread)
		{
			task->Execute();

			const float progress = finishedTasks.load() / (float)numTasks;

			if (progress - lastPrg > 0.05f)
			{
				SAILOR_LOG("PathTracer Progress: %.2f", progress);
				lastPrg = progress;
			}
		}
		SAILOR_PROFILE_END_BLOCK();

		SAILOR_PROFILE_BLOCK("Wait all calcs");
		for (auto& task : tasks)
		{
			if (!task->IsFinished())
			{
				task->Wait();

				const float progress = finishedTasks.load() / (float)numTasks;
				if (progress - lastPrg > 0.05f)
				{
					SAILOR_LOG("PathTracer Progress: %.2f", progress);
					lastPrg = progress;
				}
			}
		}
		SAILOR_PROFILE_END_BLOCK();
	}

	raytracingTimer.Stop();
	//profiler::dumpBlocksToFile("test_profile.prof");

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

	char msg[2048];
	if (raytracingTimer.ResultMs() > 10000.0f)
	{
		sprintf_s(msg, "Time in sec: %.2f", (float)raytracingTimer.ResultMs() * 0.001f);
		MessageBoxA(0, msg, msg, 0);
	}

	::system(params.m_output.string().c_str());
}

vec3 PathTracer::Raytrace(const Math::Ray& ray, const BVH& bvh, uint32_t bounceLimit, uint32_t ignoreTriangle, const PathTracer::Params& params) const
{
	SAILOR_PROFILE_FUNCTION();

	vec3 res = vec3(0);
	RaycastHit hit;

	if (bvh.IntersectBVH(ray, hit, 0, std::numeric_limits<float>().max(), ignoreTriangle))
	{
		SAILOR_PROFILE_BLOCK("Sampling");

		const Math::Triangle& tri = m_triangles[hit.m_triangleIndex];

		vec3 faceNormal = vec3(hit.m_barycentricCoordinate.x * tri.m_normals[0] + hit.m_barycentricCoordinate.y * tri.m_normals[1] + hit.m_barycentricCoordinate.z * tri.m_normals[2]);
		const vec3 tangent = vec3(hit.m_barycentricCoordinate.x * tri.m_tangent[0] + hit.m_barycentricCoordinate.y * tri.m_tangent[1] + hit.m_barycentricCoordinate.z * tri.m_tangent[2]);
		const vec3 bitangent = vec3(hit.m_barycentricCoordinate.x * tri.m_bitangent[0] + hit.m_barycentricCoordinate.y * tri.m_bitangent[1] + hit.m_barycentricCoordinate.z * tri.m_bitangent[2]);

		if (dot(faceNormal, ray.GetDirection()) > 0.0f)
		{
			faceNormal *= -1.0f;
		}

		const mat3 tbn(tangent, bitangent, faceNormal);

		const vec2 uv = hit.m_barycentricCoordinate.x * tri.m_uvs[0] +
			hit.m_barycentricCoordinate.y * tri.m_uvs[1] +
			hit.m_barycentricCoordinate.z * tri.m_uvs[2];

		//const auto& material = m_materials[tri.m_materialIndex];

		const LightingModel::SampledData sample = GetSampledData(tri.m_materialIndex, uv);
		const vec3 viewDirection = -normalize(ray.GetDirection());
		const vec3 worldNormal = normalize(tbn * sample.m_normal);

		const bool bMirror = sample.m_orm.y <= 0.001f;
		const bool bFullDielectric = sample.m_orm.z == 0.0f;
		const bool bFullMetallic = sample.m_orm.z == 1.0f;
		const bool bOnlySpecularRay = (bFullDielectric || bFullMetallic) && bMirror;
		const bool bHasTransmission = !bFullMetallic && sample.m_transmission > 0.0f;

		const bool bHasAlphaBlending = !sample.m_bIsOpaque && sample.m_baseColor.a < 1.0f;
		const uint32_t numSamples = bHasAlphaBlending ? std::max(1u, (uint32_t)round(sample.m_baseColor.a * (float)params.m_numSamples)) : params.m_numSamples;

		RaycastHit hitLight{};
		vec3 offset = 0.000001f * faceNormal;

		SAILOR_PROFILE_END_BLOCK();

		for (uint32_t i = 0; i < m_directionalLights.Num(); i++)
		{
			SAILOR_PROFILE_BLOCK("Direct lighting");
			const vec3 toLight = -m_directionalLights[i].m_direction;
			Ray rayToLight(hit.m_point + offset, toLight);
			if (!bvh.IntersectBVH(rayToLight, hitLight, 0, std::numeric_limits<float>().max(), hit.m_triangleIndex))
			{
				const float angle = max(0.0f, glm::dot(toLight, worldNormal));
				res += LightingModel::CalculateBRDF(viewDirection, worldNormal, toLight, sample) * m_directionalLights[i].m_intensity * angle;
			}
			SAILOR_PROFILE_END_BLOCK();
		}

		if (bounceLimit > 0)
		{
			SAILOR_PROFILE_BLOCK("Bounces");

			vec3 indirect = vec3(0.0f, 0.0f, 0.0f);

			const uint32_t numExtraSamples = bounceLimit == params.m_numBounces ? numSamples : 1;
			float contribution = 0.0f;

			for (uint32_t i = 0; i < numExtraSamples; i++)
			{
				const bool bSpecular = bOnlySpecularRay || glm::linearRand(0.0f, 1.0f) > 0.5f;
				const bool bTransmission = bHasTransmission && (glm::linearRand(0.0f, 1.0f) > 0.5f);

				const float importanceRoughness = bSpecular ? sample.m_orm.y : 1.0f;

				vec2 randomSample = vec2(glm::linearRand(0.0f, 1.0f), glm::linearRand(0.0f, 1.0f));
				vec3 H = LightingModel::ImportanceSampleGGX(randomSample, importanceRoughness, worldNormal);
				vec3 direction = 2.0f * dot(viewDirection, H) * H - viewDirection;

				if (bTransmission)
				{
					direction += 2.0f * worldNormal * dot(-direction, worldNormal);
				}

				const float pdfSpec = LightingModel::GGX_PDF(worldNormal, H, viewDirection, sample.m_orm.y);
				const float pdfLambert = abs(dot(direction, worldNormal)) / Math::Pi;

				float pdf = bOnlySpecularRay ? pdfSpec : ((pdfSpec + pdfLambert) * 0.5f);

				if (bHasTransmission)
				{
					pdf *= 0.5f;
				}

				if (!isnan(pdf) && pdf > 0.0001f)
				{
					const float angle = abs(glm::dot(direction, worldNormal));

					vec3 term = vec3(0.0f, 0.0f, 0.0f);
					if (bTransmission)
					{
						offset = -0.00001f * faceNormal;
						//const vec3 lDirection = lightDirection - 2.0f * worldNormal * dot(-lightDirection, worldNormal);
						term = LightingModel::CalculateBTDF(viewDirection, worldNormal, direction, sample);
					}
					else
					{
						term = LightingModel::CalculateBRDF(viewDirection, worldNormal, direction, sample);
					}

					const float weight = 1.0f / pdf;
					const Ray rayToDirection(hit.m_point + offset, direction);

					indirect += glm::clamp(
						weight *
						term *
						angle *
						Raytrace(rayToDirection, bvh, bounceLimit - 1, hit.m_triangleIndex, params),
						vec3(0.0f, 0.0f, 0.0f),
						vec3(10.0f, 10.0f, 10.0f));

					contribution += 1.0f;
				}
			}

			if (contribution > 0.0f)
			{
				res += /* sample.m_orm.r */ (indirect / contribution);
			}

			SAILOR_PROFILE_END_BLOCK();
		}

		res += sample.m_emissive;

		// Alpha Blending
		if (bounceLimit > 0 && bHasAlphaBlending)
		{
			Math::Ray newRay{};
			newRay.SetDirection(ray.GetDirection());
			newRay.SetOrigin(hit.m_point + ray.GetDirection() * 0.0001f);

			PathTracer::Params p = params;
			p.m_numBounces = std::max(0u, params.m_numBounces - 1);
			p.m_numSamples = std::max(1u, params.m_numSamples - numSamples);

			res = res * sample.m_baseColor.a +
				Raytrace(newRay, bvh, bounceLimit - 1, hit.m_triangleIndex, p) * (1.0f - sample.m_baseColor.a);
		}
	}
	else
	{
		/*
		for (uint32_t i = 0; i < m_directionalLights.Num(); i++)
		{
			SAILOR_PROFILE_BLOCK("Direct lighting for ambient");
			const vec3 toLight = -m_directionalLights[i].m_direction;
			res += m_directionalLights[i].m_intensity * angle;

			SAILOR_PROFILE_END_BLOCK();
		}*/

		res = max(res, params.m_ambient);
	}

	return res;
}

LightingModel::SampledData PathTracer::GetSampledData(const size_t& materialIndex, glm::vec2 uv) const
{
	const auto& material = m_materials[materialIndex];

	LightingModel::SampledData res{};
	res.m_baseColor = material.m_baseColorFactor;
	res.m_normal = vec3(0, 0, 1.0f);
	res.m_orm = vec3(0.0f, material.m_roughnessFactor, material.m_metallicFactor);
	res.m_emissive = material.m_emissiveFactor;
	res.m_transmission = material.m_transmissionFactor;
	res.m_bIsOpaque = material.m_blendMode == BlendMode::Opaque;

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

	if (material.HasTransmissionTexture())
	{
		res.m_transmission *= m_textures[material.m_transmissionIndex]->Sample<vec3>(uv).r;
	}

	if (material.m_blendMode == BlendMode::Mask)
	{
		res.m_baseColor.a = res.m_baseColor.a > material.m_alphaCutoff;
	}

	return res;
}