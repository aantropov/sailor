#include "PathTracer.h"
#include "Tasks/Scheduler.h"
#include "Core/LogMacros.h"
#include "Core/Utils.h"
#include "glm/glm/glm.hpp"
#include "Math/Math.h"
#include "Math/Bounds.h"
#include "Core/StringHash.h"
#include "glm/glm/gtc/random.hpp"
#include "glm/glm/gtx/matrix_transform_2d.hpp"

#include "stb/stb_image.h"

#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define __STDC_LIB_EXT1__
#include "stb/stb_image_write.h"
#endif 

//#include "assimp/scene.h"
//#include "assimp/postprocess.h"
//#include "assimp/Importer.hpp"
//#include "assimp/DefaultLogger.hpp"
//#include "assimp/LogStream.hpp"

using namespace Sailor;
using namespace Sailor::Math;
using namespace Sailor::Raytracing;

void PathTracer::ParseCommandLineArgs(PathTracer::Params& res, const char** args, int32_t num)
{
	for (int32_t i = 1; i < num; i++)
	{
		std::string arg = args[i];

		if (arg == "--in")
		{
			res.m_pathToModel = Utils::GetArgValue(args, i, num);
		}
		else if (arg == "--out")
		{
			res.m_output = Utils::GetArgValue(args, i, num);
		}
		else if (arg == "--height")
		{
			res.m_height = atoi(Utils::GetArgValue(args, i, num).c_str());
		}
		else if (arg == "--samples")
		{
			const uint32_t samples = atoi(Utils::GetArgValue(args, i, num).c_str());

			res.m_msaa = samples <= 32 ? std::min(4u, samples) : 8u;
			res.m_numSamples = std::max(1u, (uint32_t)std::lround(samples / (float)res.m_msaa));
		}
		else if (arg == "--bounces")
		{
			res.m_maxBounces = atoi(Utils::GetArgValue(args, i, num).c_str());
		}
		else if (arg == "--camera")
		{
			res.m_camera = Utils::GetArgValue(args, i, num);
		}
		else if (arg == "--ambient")
		{
			const std::string& hexStr = Utils::GetArgValue(args, i, num);
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
	/*
	Utils::Timer raytracingTimer;
	raytracingTimer.Start();

	const uint32_t GroupSize = 32;

	Assimp::Importer importer;

	const unsigned int DefaultImportFlags_Assimp =
		aiProcess_FlipUVs |
		aiProcess_GenNormals |
		aiProcess_GenUVCoords |
		0;

	const auto scene = importer.ReadFile(params.m_pathToModel.string().c_str(), DefaultImportFlags_Assimp);

	if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
	{
		SAILOR_LOG("%s", importer.GetErrorString());
		return;
	}

	ensure(scene->HasCameras(), "Scene %s has no Cameras!", params.m_pathToModel.string().c_str());

	// Camera
	auto cameraPos = vec3(0, 0.75f, 5.0f);

	auto cameraUp = normalize(vec3(0, 1, 0));
	auto cameraForward = normalize(-cameraPos);

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
		: glm::radians(60.0f);

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

			//SAILOR_LOG("\n\n\n");

			// Parse specific properties
			for (uint32_t j = 0; j < aiMaterial->mNumProperties; j++)
			{
				const auto& prop = aiMaterial->mProperties[j];
				//SAILOR_LOG("Prop: %s", prop->mKey.C_Str());
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
				else if (strcmp("$mat.refracti", prop->mKey.C_Str()) == 0)
				{
					check(prop->mType == aiPropertyTypeInfo::aiPTI_Float);
					memcpy(&material.m_indexOfRefraction, prop->mData, 4);
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

			material.m_thicknessFactor = aiMaterial->Get(AI_MATKEY_VOLUME_THICKNESS_FACTOR, color1D) == AI_SUCCESS ? color1D : 0.0f;
			material.m_attenuationColor = (aiMaterial->Get(AI_MATKEY_VOLUME_ATTENUATION_COLOR, color3D) == AI_SUCCESS) ? vec3(color3D.r, color3D.g, color3D.b) : glm::vec3(1.0f);
			material.m_attenuationDistance = aiMaterial->Get(AI_MATKEY_VOLUME_ATTENUATION_DISTANCE, color1D) == AI_SUCCESS ? color1D : std::numeric_limits<float>().max();

			aiUVTransform uvTransform{};

			if (aiMaterial->Get(AI_MATKEY_UVTRANSFORM_DIFFUSE(0), uvTransform) == AI_SUCCESS)
			{
				struct transform
				{
					float offsetX{}, offsetY{};
					float scaleX{}, scaleY{};
					float rotation{};
				};

				transform t;
				memcpy(&t, &uvTransform, sizeof(float) * 5);

				glm::mat3 scale = mat3(t.scaleX, 0, 0, 0, t.scaleY, 0, 0, 0, 1);
				glm::mat3 translation = mat3(1, 0, 0, 0, 1, 0, t.offsetX, t.offsetY, 1);
				glm::mat3 rotation = mat3(
					cos(t.rotation), -sin(t.rotation), 0,
					sin(t.rotation), cos(t.rotation), 0,
					0, 0, 1
				);

				material.m_uvTransform = translation * rotation * scale;
			}
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
	

	BVH bvh((uint32_t)m_triangles.Num());
	bvh.BuildBVH(m_triangles);

	CombinedSampler2D outputTex;
	outputTex.Initialize<vec3>(width, height);

	float h = tan(vFov / 2);
	const float ViewportHeight = 2.0f * h;
	const float ViewportWidth = aspectRatio * ViewportHeight;
	
	vec3 _u = normalize(cross(cameraUp, -cameraForward));
	vec3 _v = cross(-cameraForward, _u);

	const vec3 ViewportU = ViewportWidth * _u;
	const vec3 ViewportV = ViewportHeight * _v;
	const vec3 ViewportPivot = cameraPos - (ViewportU + ViewportV) * 0.5f + cameraForward;

	const vec3 _pixelDeltaU = ViewportU / (float)width;
	const vec3 _pixelDeltaV = ViewportV / (float)height;
	const vec3 _pixel00Dir = ViewportPivot + 0.5f * (_pixelDeltaU + _pixelDeltaV) - cameraPos;

	// Raytracing
	{
		SAILOR_PROFILE_SCOPE("Prepare raytracing tasks");

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
					&outputTex,
					&bvh,
					this]() mutable
					{
						Ray ray;
						ray.SetOrigin(cameraPos);

#ifdef _DEBUG
						uint32_t debugX = 975u;
						uint32_t debugY = height - 355u - 1;

						if (!(x < debugX && (x + GroupSize) > debugX &&
							y < debugY && (y + GroupSize) > debugY))
						{
							return;
						}
#endif
						for (uint32_t v = 0; (v < GroupSize) && (y + v) < height; v++)
						{
							const float tv = (y + v) / (float)height;
							for (uint32_t u = 0; u < GroupSize && (u + x) < width; u++)
							{
								SAILOR_PROFILE_SCOPE("Raycasting");

								const uint32_t index = (height - (y + v) - 1) * width + (x + u);
								const float tu = (x + u) / (float)width;
#ifdef _DEBUG
								if (((x + u) == debugX) && ((y + v) == debugY))
								{
									volatile uint8_t a = 0;
								}
#endif
								vec3 accumulator = vec3(0);
								for (uint32_t sample = 0; sample < params.m_msaa; sample++)
								{
									const vec2 offset = sample == 0 ? vec2(0.5f, 0.5f) : glm::linearRand(vec2(0, 0), vec2(1.0f, 1.0f));
									const vec3 pixelDir = _pixel00Dir + ((float)(u + x) + offset.x) * _pixelDeltaU + ((float)(y + v) - offset.y) * _pixelDeltaV;

									ray.SetDirection(glm::normalize(pixelDir));

									accumulator += Raytrace(ray, bvh, params.m_maxBounces, (uint32_t)(-1), params, 1.0f, 1.0f);
								}

								vec3 res = accumulator / (float)params.m_msaa;
								outputTex.SetPixel(x + u, height - (y + v) - 1, res);
								}
							}

						finishedTasks++;

					}, EThreadType::Worker);

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

		float lastPrg = 0.0f;
		{
			SAILOR_PROFILE_SCOPE("Calcs on Main thread");
			float eta = 0.0f;
			for (auto& task : tasksThisThread)
			{
				task->Execute();

				const float progress = finishedTasks.load() / (float)numTasks;

				if (progress - lastPrg > 0.05f)
				{
					if (eta == 0.0f)
					{
						eta = raytracingTimer.ResultAccumulatedMs() * 20.0f * 0.001f * 1.5f;
						SAILOR_LOG("PathTracer ETA: ~%.2fsec (%.2fmin)", eta, round(eta / 60.0f));
					}

					SAILOR_LOG("PathTracer Progress: %.2f", progress);
					lastPrg = progress;
				}
			}
		}

		{
			SAILOR_PROFILE_SCOPE("Wait all calcs");
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
		}
	}

	raytracingTimer.Stop();
	//profiler::dumpBlocksToFile("test_profile.prof"_h);

	{
		SAILOR_PROFILE_SCOPE("Write Image");

		TVector<u8vec3> outSrgb(width * height);
		const float aberrationAmount = (0.5f / width);

		for (uint32_t y = 0; y < height; y++)
		{
			for (uint32_t x = 0; x < width; x++)
			{
				vec2 uv = vec2((float)x / width, (float)y / height);

				// Fetch the color values from the offset positions
				vec3 greenColor = outputTex.Sample<vec3>(uv + vec2(aberrationAmount, 0));
				vec3 blueColor = outputTex.Sample<vec3>(uv + vec2(aberrationAmount, aberrationAmount));
				vec3 redColor = outputTex.Sample<vec3>(uv + vec2(-aberrationAmount, -aberrationAmount));

				// Combine the shifted channels
				vec3 chromaAberratedColor = vec3(redColor.r, greenColor.g, blueColor.b);

				// Write the result back to the output array
				outSrgb[x + y * width] = glm::clamp(Utils::LinearToSRGB(chromaAberratedColor) * 255.0f, 0.0f, 255.0f);
			}
		}

		const uint32_t Channels = 3;
		if (!stbi_write_png(params.m_output.string().c_str(), width, height, Channels, outSrgb.GetData(), width * Channels))
		{
			SAILOR_LOG("Raytracing WriteImage error");
		}
	}

	char msg[2048];
	if (raytracingTimer.ResultMs() > 10000.0f)
	{
		sprintf_s(msg, "Time in sec: %.2f", (float)raytracingTimer.ResultMs() * 0.001f);
		MessageBoxA(0, msg, msg, 0);
	}

	::system(params.m_output.string().c_str());*/
}

vec3 PathTracer::TraceSky(vec3 startPoint, vec3 toLight, const BVH& bvh, const PathTracer::Params& params, float currentIor, uint32_t ignoreTriangle) const
{
	vec3 att = vec3(1, 1, 1);
	vec3 prevHitPoint = startPoint;
	for (uint32_t j = 0; j < params.m_maxBounces; j++)
	{
		RaycastHit hitLight{};
		Ray rayToLight(startPoint, toLight);

		if (!bvh.IntersectBVH(rayToLight, hitLight, 0, std::numeric_limits<float>().max(), ignoreTriangle))
		{
			return att;
		}

		const auto& material = m_materials[m_triangles[hitLight.m_triangleIndex].m_materialIndex];

		const bool bHitOpposite = dot(toLight, hitLight.m_normal) < 0.0f;
		const bool bHitThickVolume = material.m_transmissionFactor > 0.0f && material.m_thicknessFactor > 0.0f;

		if (!bHitThickVolume)
		{
			return vec3(0, 0, 0);
		}

		const float distance = length(hitLight.m_point - prevHitPoint);
		prevHitPoint = hitLight.m_point;

		vec3 hitWorldNormal = bHitOpposite ? hitLight.m_normal : -hitLight.m_normal;

		toLight = LightingModel::CalculateRefraction(toLight, hitWorldNormal, currentIor, bHitOpposite ? material.m_indexOfRefraction : 1.0f);
		currentIor = bHitOpposite ? material.m_indexOfRefraction : 1.0f;

		if (!bHitOpposite)
		{
			const vec3 c = -log(material.m_attenuationColor) / material.m_attenuationDistance;
			att *= glm::exp(-c * distance);
		}

		startPoint = hitLight.m_point;
		ignoreTriangle = hitLight.m_triangleIndex;
	}

	return vec3(0, 0, 0);
}

vec3 PathTracer::Raytrace(const Math::Ray& ray, const BVH& bvh, uint32_t bounceLimit, uint32_t ignoreTriangle, const PathTracer::Params& params, float inAcc, float environmentIor) const
{
	SAILOR_PROFILE_FUNCTION();

	uint32_t randSeedX = glm::linearRand(0, 680);
	uint32_t randSeedY = glm::linearRand(0, 680);

	vec3 res = vec3(0);
	RaycastHit hit;

	if (bvh.IntersectBVH(ray, hit, 0, std::numeric_limits<float>().max(), ignoreTriangle))
	{
		SAILOR_PROFILE_SCOPE("Sampling");

		const bool bIsFirstIntersection = bounceLimit == params.m_maxBounces;

		const Math::Triangle& tri = m_triangles[hit.m_triangleIndex];

		vec3 faceNormal = vec3(hit.m_barycentricCoordinate.x * tri.m_normals[0] + hit.m_barycentricCoordinate.y * tri.m_normals[1] + hit.m_barycentricCoordinate.z * tri.m_normals[2]);
		const vec3 tangent = vec3(hit.m_barycentricCoordinate.x * tri.m_tangent[0] + hit.m_barycentricCoordinate.y * tri.m_tangent[1] + hit.m_barycentricCoordinate.z * tri.m_tangent[2]);
		const vec3 bitangent = vec3(hit.m_barycentricCoordinate.x * tri.m_bitangent[0] + hit.m_barycentricCoordinate.y * tri.m_bitangent[1] + hit.m_barycentricCoordinate.z * tri.m_bitangent[2]);

		const bool bIsOppositeRay = dot(faceNormal, ray.GetDirection()) < 0.0f;
		if (!bIsOppositeRay)
		{
			faceNormal *= -1.0f;
		}

		const mat3 tbn(tangent, bitangent, faceNormal);

		const vec2 uv = hit.m_barycentricCoordinate.x * tri.m_uvs[0] +
			hit.m_barycentricCoordinate.y * tri.m_uvs[1] +
			hit.m_barycentricCoordinate.z * tri.m_uvs[2];

		const auto material = m_materials[tri.m_materialIndex];
		const vec2 uvTransformed = (material.m_uvTransform * vec3(uv, 1));

		const LightingModel::SampledData sample = GetMaterialData(tri.m_materialIndex, uvTransformed);
		const vec3 viewDirection = -normalize(ray.GetDirection());
		const vec3 worldNormal = normalize(tbn * sample.m_normal);

		const bool bHasAlphaBlending = !sample.m_bIsOpaque && sample.m_baseColor.a < 1.0f;
		const uint32_t numSamples = bHasAlphaBlending ? std::max(1u, (uint32_t)round(sample.m_baseColor.a * (float)params.m_numSamples)) : params.m_numSamples;
		const uint32_t numAmbientSamples = bHasAlphaBlending ? std::max(1u, (uint32_t)round(sample.m_baseColor.a * (float)params.m_numAmbientSamples)) : params.m_numAmbientSamples;

		const vec3 offset = 0.000001f * faceNormal;

		const bool bFullMetallic = sample.m_orm.z == 1.0f;
		const bool bHasTransmission = !bFullMetallic && sample.m_transmission > 0.0f;
		const bool bThickVolume = bHasTransmission && material.m_thicknessFactor > 0.0f;

		if (!bIsOppositeRay && bThickVolume)
		{
			const vec3 newDirection = LightingModel::CalculateRefraction(ray.GetDirection(), worldNormal, environmentIor, 1.0f);

			if (newDirection == vec3(0, 0, 0) || bounceLimit == 0)
			{
				return vec3(0, 0, 0);
			}

			Ray rayToLight(hit.m_point, newDirection - offset);

			//const float angle = abs(glm::dot(newDirection, worldNormal));
			//vec3 term = LightingModel::CalculateVolumetricBTDF(viewDirection, worldNormal, newDirection, sample, environmentIor) * angle;

			return Raytrace(rayToLight, bvh, bounceLimit - 1, hit.m_triangleIndex, params, inAcc, 1.0f);
		}

		// Direct lighting
		{
			SAILOR_PROFILE_SCOPE("Direct lighting");

			RaycastHit hitLight{};
			for (uint32_t i = 0; i < m_directionalLights.Num(); i++)
			{
				const vec3 toLight = -m_directionalLights[i].m_direction;
				Ray rayToLight(hit.m_point + offset, toLight);
				if (!bvh.IntersectBVH(rayToLight, hitLight, 0, std::numeric_limits<float>().max(), hit.m_triangleIndex))
				{
					const float angle = max(0.0f, glm::dot(toLight, worldNormal));
					res += LightingModel::CalculateBRDF(viewDirection, worldNormal, toLight, sample) * m_directionalLights[i].m_intensity * angle;
				}
			}
		}

		// Ambient lighting
		if (params.m_ambient.x + params.m_ambient.y + params.m_ambient.z > 0.0f)
		{
			SAILOR_PROFILE_SCOPE("Ambient lighting");

			// Random ray
			vec3 ambient1 = vec3(0, 0, 0);
			const float pdfHemisphere = 1.0f / (Pi * 2.0f);

			const uint32_t ambientNumSamples = bIsFirstIntersection ? numAmbientSamples : 1u;
			const uint32_t numExtraSamples = bIsFirstIntersection ? numSamples : 1;

			// Hemisphere sampling loop
			if (!bThickVolume)
			{
				for (uint32_t i = 0; i < ambientNumSamples; i++)
				{
					const vec2 randomSample = NextVec2_Linear();
					vec3 H = LightingModel::ImportanceSampleHemisphere(randomSample, worldNormal);
					vec3 toLight = bThickVolume ? glm::sphericalRand(1.0f) : (2.0f * dot(viewDirection, H) * H - viewDirection);

					vec3 att = TraceSky(hit.m_point + offset, toLight, bvh, params, environmentIor, hit.m_triangleIndex);
					if (att != vec3(0, 0, 0))
					{
						const float angle = max(0.0f, glm::dot(toLight, worldNormal));
						att *= LightingModel::CalculateBRDF(viewDirection, worldNormal, toLight, sample);
						ambient1 += glm::clamp((att * params.m_ambient * angle) / pdfHemisphere,
							vec3(0, 0, 0), vec3(10, 10, 10));
					}
				}
			}

			ambient1 /= (float)ambientNumSamples;

			// Importance sampling ray
			vec3 ambient2 = vec3(0, 0, 0);
			float avgPdfLambert = 0.0f;

			// Indirect lighting
			vec3 indirect = vec3(0.0f, 0.0f, 0.0f);
			float indirectContribution = 0.0f;

			const float toIor = bThickVolume ? (bIsOppositeRay ? sample.m_ior : 1.0f) : environmentIor;
			bool bHasTransmissionRay = false;

			// Importance sampling loop
			for (uint32_t i = 0; i < numExtraSamples; i++)
			{
				vec3 term{};
				float pdf = 0.0f;
				bool bTransmissionRay = false;
				vec3 direction = vec3(0);
				bool bSample = false;

				while (!bSample || (bThickVolume && !bHasTransmissionRay && i == (numExtraSamples - 1)))
				{
					direction = vec3(0);
					const vec2 randomSample = NextVec2_BlueNoise(randSeedX, randSeedY);
					bSample = LightingModel::Sample(sample, worldNormal, viewDirection, environmentIor, toIor, term, pdf, bTransmissionRay, direction, randomSample);
					bHasTransmissionRay |= bTransmissionRay;
				}

				float newEnvironmentIor = environmentIor;

				if (bIsOppositeRay && bTransmissionRay && bThickVolume)
				{
					newEnvironmentIor = sample.m_ior;
				}
				else if (!bIsOppositeRay && bTransmissionRay && bThickVolume)
				{
					newEnvironmentIor = 1.0f;
				}

				RaycastHit hitLight{};
				Ray rayToLight(hit.m_point + (bTransmissionRay ? -offset : offset), direction);

				//vec3 att = TraceSky(rayToLight.GetOrigin(), rayToLight.GetDirection(), bvh, params, environmentIor, hit.m_triangleIndex);
				//const bool bSkyTraced = length(att) > 0.0f;

				if (!bvh.IntersectBVH(rayToLight, hitLight, 0, std::numeric_limits<float>().max(), hit.m_triangleIndex))
				{
					vec3 value = glm::clamp(term * params.m_ambient, vec3(0, 0, 0), vec3(10, 10, 10));

					// Ambient lighting
					ambient2 += value;
					avgPdfLambert += pdf;

					// Indirect lighting with the correct pdf in case of miss
					indirect += value;
				}
				else if (bounceLimit > 0)
				{
					// Indirect
					vec3 lightAttenuation = vec3(1, 1, 1);
					if (bIsOppositeRay && bTransmissionRay && bThickVolume)
					{
						const float distance = glm::length(hitLight.m_point - hit.m_point);
						const vec3 c = -log(material.m_attenuationColor) / material.m_attenuationDistance;

						lightAttenuation = glm::exp(-c * distance);
					}

					vec3 raytraced = vec3(0, 0, 0);
					const float newAcc = inAcc * length(term * lightAttenuation) * sample.m_baseColor.a;
					if (newAcc > 0.01f)
					{
						raytraced = Raytrace(rayToLight, bvh, bounceLimit - 1, hit.m_triangleIndex, params, newAcc, newEnvironmentIor);
					}

					vec3 value = glm::clamp(term * lightAttenuation * raytraced, vec3(0, 0, 0), vec3(10, 10, 10));

					// Indirect lighting with bounces in case of hit
					indirect += value;

					// Ambient 2, Sky is reachable
					const auto& hitMaterial = m_materials[m_triangles[hitLight.m_triangleIndex].m_materialIndex];
					if (!bThickVolume && hitMaterial.m_transmissionFactor > 0.0f && hitMaterial.m_thicknessFactor > 0.0f)
					{
						vec3 att = TraceSky(rayToLight.GetOrigin(), rayToLight.GetDirection(), bvh, params, environmentIor, hitLight.m_triangleIndex);

						if (att != vec3(0, 0, 0))
						{
							ambient2 += value * att;
							avgPdfLambert += pdf;
						}
					}
				}

				indirectContribution += 1.0f;
			}

			ambient2 /= (float)indirectContribution;
			avgPdfLambert /= (float)indirectContribution;

			const vec3 ambient = ambient1 + ambient2;
			if (ambient.x + ambient.y + ambient.z > 0.0f)
			{
				const vec3 combinedAmbient = ambient1 * LightingModel::PowerHeuristic(ambientNumSamples, pdfHemisphere, (int32_t)indirectContribution, avgPdfLambert) +
					ambient2 * LightingModel::PowerHeuristic((int32_t)indirectContribution, avgPdfLambert, ambientNumSamples, pdfHemisphere);
				res += combinedAmbient;
			}

			if (indirectContribution > 0.0f)
			{
				res += (indirect / indirectContribution);
			}
		}

		res += sample.m_emissive;

		// Alpha Blending
		if (bounceLimit > 0 && bHasAlphaBlending)
		{
			Math::Ray newRay{};
			newRay.SetDirection(ray.GetDirection());
			newRay.SetOrigin(hit.m_point + ray.GetDirection() * 0.0001f);

			PathTracer::Params p = params;
			p.m_maxBounces = std::max(0u, params.m_maxBounces - 1);
			p.m_numSamples = std::max(1u, params.m_numSamples - numSamples);
			p.m_numAmbientSamples = std::max(1u, params.m_numAmbientSamples - numAmbientSamples);

			res = res * sample.m_baseColor.a +
				Raytrace(newRay, bvh, bounceLimit - 1, hit.m_triangleIndex, p, inAcc * (1.0f - sample.m_baseColor.a), environmentIor) * (1.0f - sample.m_baseColor.a);
		}
	}
	else
	{
		res = params.m_ambient;
	}

	return res;
}

LightingModel::SampledData PathTracer::GetMaterialData(const size_t& materialIndex, glm::vec2 uv) const
{
	const auto& material = m_materials[materialIndex];

	LightingModel::SampledData res{};
	res.m_baseColor = material.m_baseColorFactor;
	res.m_normal = vec3(0, 0, 1.0f);
	res.m_orm = vec3(0.0f, material.m_roughnessFactor, material.m_metallicFactor);
	res.m_emissive = material.m_emissiveFactor;
	res.m_transmission = material.m_transmissionFactor;
	res.m_bIsOpaque = material.m_blendMode == BlendMode::Opaque;
	res.m_thicknessFactor = material.m_thicknessFactor;
	res.m_ior = material.m_indexOfRefraction;

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

vec2 PathTracer::NextVec2_Linear()
{
	return vec2(glm::linearRand(0.0f, 1.0f), glm::linearRand(0.0f, 1.0f));
}

vec2 PathTracer::NextVec2_BlueNoise(uint32_t& randSeedX, uint32_t& randSeedY)
{
	/*
	static const vec2 BlueNoiseInDisk[64] = {
   vec2(0.478712, 0.875764),
   vec2(-0.337956, -0.793959),
   vec2(-0.955259, -0.028164),
   vec2(0.864527, 0.325689),
   vec2(0.209342, -0.395657),
   vec2(-0.106779, 0.672585),
   vec2(0.156213, 0.235113),
   vec2(-0.413644, -0.082856),
   vec2(-0.415667, 0.323909),
   vec2(0.141896, -0.939980),
   vec2(0.954932, -0.182516),
   vec2(-0.766184, 0.410799),
   vec2(-0.434912, -0.458845),
   vec2(0.415242, -0.078724),
   vec2(0.728335, -0.491777),
   vec2(-0.058086, -0.066401),
   vec2(0.202990, 0.686837),
   vec2(-0.808362, -0.556402),
   vec2(0.507386, -0.640839),
   vec2(-0.723494, -0.229240),
   vec2(0.489740, 0.317826),
   vec2(-0.622663, 0.765301),
   vec2(-0.010640, 0.929347),
   vec2(0.663146, 0.647618),
   vec2(-0.096674, -0.413835),
   vec2(0.525945, -0.321063),
   vec2(-0.122533, 0.366019),
   vec2(0.195235, -0.687983),
   vec2(-0.563203, 0.098748),
   vec2(0.418563, 0.561335),
   vec2(-0.378595, 0.800367),
   vec2(0.826922, 0.001024),
   vec2(-0.085372, -0.766651),
   vec2(-0.921920, 0.183673),
   vec2(-0.590008, -0.721799),
   vec2(0.167751, -0.164393),
   vec2(0.032961, -0.562530),
   vec2(0.632900, -0.107059),
   vec2(-0.464080, 0.569669),
   vec2(-0.173676, -0.958758),
   vec2(-0.242648, -0.234303),
   vec2(-0.275362, 0.157163),
   vec2(0.382295, -0.795131),
   vec2(0.562955, 0.115562),
   vec2(0.190586, 0.470121),
   vec2(0.770764, -0.297576),
   vec2(0.237281, 0.931050),
   vec2(-0.666642, -0.455871),
   vec2(-0.905649, -0.298379),
   vec2(0.339520, 0.157829),
   vec2(0.701438, -0.704100),
   vec2(-0.062758, 0.160346),
   vec2(-0.220674, 0.957141),
   vec2(0.642692, 0.432706),
   vec2(-0.773390, -0.015272),
   vec2(-0.671467, 0.246880),
   vec2(0.158051, 0.062859),
   vec2(0.806009, 0.527232),
   vec2(-0.057620, -0.247071),
   vec2(0.333436, -0.516710),
   vec2(-0.550658, -0.315773),
   vec2(-0.652078, 0.589846),
   vec2(0.008818, 0.530556),
   vec2(-0.210004, 0.519896)
	};*/

	float BlueNoiseData[] = { 0.1025390625, 0.4072265625, 0.6240234375, 0.2763671875, 0.7021484375, 0.3701171875, 0.7666015625, 0.9755859375, 0.4619140625, 0.8173828125, 0.5048828125, 0.3427734375,
		0.9677734375, 0.7431640625, 0.4287109375, 0.6875, 0.5458984375, 0.19140625, 0.47265625, 0.109375, 0.2578125, 0.509765625, 0.01171875, 0.3232421875, 0.158203125, 0.6904296875, 0.2236328125,
		0.9375, 0.1396484375, 0.8095703125, 0.4375, 0.568359375, 0.318359375, 0.787109375, 0.8837890625, 0.5234375, 0.14453125, 0.6064453125, 0.2109375, 0.11328125, 0.6865234375, 0.1708984375, 0.8984375,
		0.0341796875, 0.537109375, 0.1455078125, 0.2802734375, 0.919921875, 0.001953125, 0.3017578125, 0.9736328125, 0.58203125, 0.6943359375, 0.9306640625, 0.767578125, 0.65234375, 0.9091796875, 0.3916015625,
		0.7568359375, 0.57421875, 0.3525390625, 0.6298828125, 0.1796875, 0.8623046875, 0.7080078125, 0.2080078125, 0.0302734375, 0.4453125, 0.9267578125, 0.79296875, 0.4833984375, 0.8759765625, 0.3759765625,
		0.599609375, 0.2919921875, 0.7763671875, 0.6279296875, 0.3857421875, 0.7900390625, 0.498046875, 0.6328125, 0.8408203125, 0.7216796875, 0.4150390625, 0.0556640625, 0.208984375, 0.453125, 0.1123046875,
		0.2509765625, 0.5146484375, 0.095703125, 0.8828125, 0.0400390625, 0.98046875, 0.27734375, 0.478515625, 0.95703125, 0.5771484375, 0.3349609375, 0.734375, 0.23828125, 0.05859375, 0.306640625, 0.5517578125,
		0.01953125, 0.9609375, 0.4404296875, 0.193359375, 0.8603515625, 0.060546875, 0.9951171875, 0.2138671875, 0.1015625, 0.369140625, 0.142578125, 0.244140625, 0.853515625, 0.62109375, 0.361328125, 0.9814453125,
		0.587890625, 0.8505859375, 0.310546875, 0.4501953125, 0.7158203125, 0.525390625, 0.662109375, 0.0517578125, 0.40234375, 0.125, 0.9130859375, 0.634765625, 0.3876953125, 0.9833984375, 0.6640625, 0.8193359375,
		0.25390625, 0.751953125, 0.1171875, 0.673828125, 0.4873046875, 0.3330078125, 0.5634765625, 0.693359375, 0.44921875, 0.77734375, 0.5693359375, 0.91015625, 0.4794921875, 0.7939453125, 0.17578125, 0.74609375, 0.04296875,
		0.681640625, 0.1484375, 0.80859375, 0.2294921875, 0.376953125, 0.15625, 0.8271484375, 0.2431640625, 0.46875, 0.8076171875, 0.1787109375, 0.5322265625, 0.1103515625, 0.427734375, 0.1640625, 0.6259765625, 0.5107421875,
		0.3603515625, 0.943359375, 0.2490234375, 0.7548828125, 0.1513671875, 0.8662109375, 0.263671875, 0.958984375, 0.025390625, 0.6767578125, 0.3115234375, 0.0888671875, 0.5478515625, 0.28125, 0.4716796875, 0.3720703125, 0.966796875,
		0.609375, 0.06640625, 0.923828125, 0.7392578125, 0.6044921875, 0.873046875, 0.6708984375, 0.0205078125, 0.298828125, 0.849609375, 0.7060546875, 0.8896484375, 0.326171875, 0.91796875, 0.0693359375, 0.822265625, 0.5498046875,
		0.0126953125, 0.9052734375, 0.4130859375, 0.08203125, 0.650390625, 0.337890625, 0.529296875, 0.1611328125, 0.404296875, 0.9521484375, 0.6494140625, 0.89453125, 0.8203125, 0.2041015625, 0.5380859375, 0.2861328125, 0.76953125,
		0.4990234375, 0.302734375, 0.07421875, 0.34375, 0.5224609375, 0.748046875, 0.435546875, 0.583984375, 0.2265625, 0.0439453125, 0.763671875, 0.45703125, 0.2373046875, 0.6552734375, 0.177734375, 0.703125, 0.3076171875, 0.591796875,
		0.7978515625, 0.5029296875, 0.2001953125, 0.8876953125, 0.7109375, 0.8310546875, 0.236328125, 0.0078125, 0.4228515625, 0.1240234375, 0.720703125, 0.02734375, 0.869140625, 0.4111328125, 0.1884765625, 0.5673828125, 0.9921875, 0.134765625,
		0.21484375, 0.90625, 0.0986328125, 0.9638671875, 0.3623046875, 0.5078125, 0.60546875, 0.123046875, 0.98828125, 0.341796875, 0.4345703125, 0.837890625, 0.4853515625, 0.189453125, 0.978515625, 0.046875, 0.7470703125, 0.43359375, 0.0625,
		0.5966796875, 0.5068359375, 0.7333984375, 0.2880859375, 0.6162109375, 0.9482421875, 0.4541015625, 0.65625, 0.1162109375, 0.8359375, 0.689453125, 0.4326171875, 0.5986328125, 0.828125, 0.3828125, 0.640625, 0.181640625, 0.798828125,
		0.6787109375, 0.279296875, 0.87109375, 0.53125, 0.71875, 0.041015625, 0.9453125, 0.103515625, 0.677734375, 0.283203125, 0.3740234375, 0.619140625, 0.9423828125, 0.2998046875, 0.365234375, 0.150390625, 0.9990234375, 0.7919921875,
		0.3583984375, 0.5654296875, 0.224609375, 0.322265625, 0.9345703125, 0.275390625, 0.005859375, 0.7646484375, 0.2529296875, 0.0458984375, 0.7177734375, 0.2724609375, 0.4951171875, 0.015625, 0.9326171875, 0.1591796875, 0.3994140625,
		0.076171875, 0.7998046875, 0.2958984375, 0.6025390625, 0.38671875, 0.900390625, 0.5302734375, 0.8544921875, 0.130859375, 0.228515625, 0.779296875, 0.865234375, 0.6650390625, 0.072265625, 0.4736328125, 0.1748046875, 0.05078125,
		0.888671875, 0.7314453125, 0.5830078125, 0.50390625, 0.3642578125, 0.9462890625, 0.53515625, 0.455078125, 0.9716796875, 0.5732421875, 0.876953125, 0.3369140625, 0.4423828125, 0.7353515625, 0.8369140625, 0.6220703125, 0.19921875,
		0.505859375, 0.1474609375, 0.775390625, 0.216796875, 0.0048828125, 0.705078125, 0.462890625, 0.572265625, 0.0869140625, 0.439453125, 0.5537109375, 0.248046875, 0.8779296875, 0.7001953125, 0.810546875, 0.3798828125, 0.0927734375,
		0.1826171875, 0.806640625, 0.1083984375, 0.642578125, 0.896484375, 0.33984375, 0.1630859375, 0.0712890625, 0.7587890625, 0.658203125, 0.1044921875, 0.5712890625, 0.24609375, 0.357421875, 0.9501953125, 0.69921875, 0.87890625, 0.4462890625,
		0.6513671875, 0.3408203125, 0.8125, 0.26953125, 0.9765625, 0.73046875, 0.197265625, 0.931640625, 0.3935546875, 0.6181640625, 0.296875, 0.54296875, 0.4638671875, 0.982421875, 0.7041015625, 0.4267578125, 0.8642578125, 0.1982421875, 0.0849609375,
		0.6982421875, 0.8154296875, 0.423828125, 0.2099609375, 0.2978515625, 0.9853515625, 0.482421875, 0.037109375, 0.8583984375, 0.419921875, 0.056640625, 0.3134765625, 0.0966796875, 0.5439453125, 0.927734375, 0.111328125, 0.4013671875, 0.6396484375,
		0.0244140625, 0.3291015625, 0.7861328125, 0.1357421875, 0.0615234375, 0.9560546875, 0.2158203125, 0.013671875, 0.6484375, 0.2568359375, 0.5595703125, 0.3095703125, 0.7529296875, 0.501953125, 0.2666015625, 0.626953125, 0.9365234375, 0.5283203125,
		0.8525390625, 0.146484375, 0.6103515625, 0.744140625, 0.171875, 0.66015625, 0.578125, 0.7841796875, 0.9970703125, 0.251953125, 0.7373046875, 0.4970703125, 0.185546875, 0.8427734375, 0.513671875, 0.6669921875, 0.4560546875, 0.8486328125, 0.708984375,
		0.4169921875, 0.5927734375, 0.8251953125, 0.35546875, 0.12890625, 0.9296875, 0.0263671875, 0.4052734375, 0.998046875, 0.564453125, 0.1259765625, 0.3505859375, 0.0009765625, 0.6630859375, 0.390625, 0.8232421875, 0.3193359375, 0.9619140625, 0.265625,
		0.484375, 0.1494140625, 0.384765625, 0.630859375, 0.033203125, 0.890625, 0.5869140625, 0.2841796875, 0.9189453125, 0.099609375, 0.255859375, 0.5205078125, 0.3173828125, 0.880859375, 0.16015625, 0.73828125, 0.9013671875, 0.517578125, 0.724609375,
		0.623046875, 0.8349609375, 0.0576171875, 0.220703125, 0.8935546875, 0.7509765625, 0.466796875, 0.7822265625, 0.2412109375, 0.064453125, 0.4580078125, 0.55078125, 0.0224609375, 0.9072265625, 0.69140625, 0.83203125, 0.2021484375, 0.44140625,
		0.333984375, 0.796875, 0.13671875, 0.373046875, 0.75390625, 0.9541015625, 0.1845703125, 0.646484375, 0.048828125, 0.494140625, 0.2744140625, 0.0771484375, 0.41796875, 0.2333984375, 0.15234375, 0.3544921875, 0.6845703125, 0.4521484375,
		0.638671875, 0.2822265625, 0.09375, 0.9697265625, 0.5810546875, 0.904296875, 0.71484375, 0.205078125, 0.8017578125, 0.41015625, 0.30078125, 0.08984375, 0.5625, 0.9228515625, 0.6689453125, 0.0498046875, 0.712890625, 0.4599609375, 0.6005859375,
		0.00390625, 0.5615234375, 0.8037109375, 0.994140625, 0.37890625, 0.5703125, 0.6748046875, 0.791015625, 0.9658203125, 0.4892578125, 0.7734375, 0.9443359375, 0.1650390625, 0.8115234375, 0.3984375, 0.541015625, 0.1923828125, 0.3310546875,
		0.1298828125, 0.380859375, 0.8798828125, 0.603515625, 0.140625, 0.5126953125, 0.962890625, 0.7197265625, 0.2548828125, 0.1552734375, 0.5390625, 0.974609375, 0.2119140625, 0.86328125, 0.3251953125, 0.412109375, 0.234375, 0.1279296875, 0.7578125,
		0.921875, 0.18359375, 0.33203125, 0.044921875, 0.59765625, 0.2626953125, 0.52734375, 0.359375, 0.0234375, 0.88671875, 0.732421875, 0.6474609375, 0.8330078125, 0.490234375, 0.6826171875, 0.078125, 0.31640625, 0.7724609375, 0.64453125, 0.0068359375,
		0.3681640625, 0.4765625, 0.8701171875, 0.4091796875, 0.28515625, 0.6455078125, 0.10546875, 0.7880859375, 0.908203125, 0.7099609375, 0.4609375, 0.30859375, 0.0146484375, 0.6337890625, 0.83984375, 0.4384765625, 0.8720703125, 0.1005859375,
		0.7412109375, 0.9150390625, 0.5947265625, 0.25, 0.12109375, 0.4248046875, 0.0361328125, 0.939453125, 0.2646484375, 0.5263671875, 0.955078125, 0.4365234375, 0.2392578125, 0.884765625, 0.755859375, 0.107421875, 0.625, 0.8212890625, 0.0751953125,
		0.76171875, 0.49609375, 0.173828125, 0.5341796875, 0.083984375, 0.611328125, 0.8681640625, 0.5009765625, 0.240234375, 0.544921875, 0.1435546875, 0.669921875, 0.3154296875, 0.431640625, 0.0908203125, 0.68359375, 0.4697265625, 0.9892578125,
		0.314453125, 0.759765625, 0.595703125, 0.1533203125, 0.7275390625, 0.0478515625, 0.8291015625, 0.1669921875, 0.55859375, 0.3046875, 0.990234375, 0.201171875, 0.3466796875, 0.56640625, 0.953125, 0.39453125, 0.3037109375, 0.671875, 0.97265625,
		0.3662109375, 0.1953125, 0.6884765625, 0.951171875, 0.771484375, 0.3779296875, 0.986328125, 0.1904296875, 0.560546875, 0.8466796875, 0.29296875, 0.1572265625, 0.814453125, 0.5546875, 0.22265625, 0.400390625, 0.8447265625, 0.32421875,
		0.615234375, 0.3896484375, 0.6962890625, 0.4658203125, 0.0546875, 0.66796875, 0.5087890625, 0.736328125, 0.0322265625, 0.2314453125, 0.6201171875, 0.875, 0.021484375, 0.2607421875, 0.82421875, 0.0654296875, 0.421875, 0.119140625, 0.294921875,
		0.029296875, 0.61328125, 0.794921875, 0.0537109375, 0.388671875, 0.947265625, 0.5166015625, 0.0087890625, 0.70703125, 0.9248046875, 0.0830078125, 0.4814453125, 0.9775390625, 0.1943359375, 0.8916015625, 0.1201171875, 0.7802734375, 0.9140625,
		0.3837890625, 0.1318359375, 0.93359375, 0.4482421875, 0.8388671875, 0.1142578125, 0.7421875, 0.40625, 0.4931640625, 0.5751953125, 0.7607421875, 0.916015625, 0.5244140625, 0.7255859375, 0.8564453125, 0.4921875, 0.2685546875, 0.716796875,
		0.2177734375, 0.7490234375, 0.62890625, 0.3388671875, 0.447265625, 0.1767578125, 0.7958984375, 0.6533203125, 0.017578125, 0.533203125, 0.267578125, 0.5859375, 0.349609375, 0.212890625, 0.576171875, 0.84765625, 0.2705078125, 0.6806640625,
		0.3212890625, 0.5185546875, 0.1962890625, 0.90234375, 0.701171875, 0.1328125, 0.232421875, 0.6376953125, 0.34765625, 0.1806640625, 0.4140625, 0.126953125, 0.8974609375, 0.59375, 0.474609375, 0.1064453125, 0.8671875, 0.2470703125, 0.8994140625,
		0.6015625, 0.3671875, 0.2783203125, 0.7626953125, 0.4189453125, 0.7265625, 0.9599609375, 0.0419921875, 0.81640625, 0.443359375, 0.0673828125, 0.6142578125, 0.169921875, 0.7890625, 0.9873046875, 0.5849609375, 0.0810546875, 0.3125, 0.96875,
		0.46484375, 0.0029296875, 0.833984375, 0.556640625, 0.9580078125, 0.654296875, 0.3515625, 0.9794921875, 0.1728515625, 0.8056640625, 0.4208984375, 0.0595703125, 0.6953125, 0.09765625, 0.5400390625, 0.94140625, 0.138671875, 0.8515625, 0.091796875,
		0.4755859375, 0.6416015625, 0.291015625, 0.7236328125, 0.9033203125, 0.3564453125, 0.5, 0.009765625, 0.416015625, 0.26171875, 0.6591796875, 0.841796875, 0.3818359375, 0.78125, 0.287109375, 0.6796875, 0.0791015625, 0.23046875, 0.783203125, 0.0185546875,
		0.271484375, 0.6728515625, 0.330078125, 0.552734375, 0.970703125, 0.4912109375, 0.818359375, 0.21875, 0.67578125, 0.3447265625, 0.607421875, 0.2216796875, 0.375, 0.92578125, 0.154296875, 0.5361328125, 0.203125, 0.8046875, 0.9287109375, 0.6923828125,
		0.1416015625, 0.7744140625, 0.4775390625, 0.0380859375, 0.6123046875, 0.1689453125, 0.9404296875, 0.48828125, 0.8955078125, 0.3203125, 0.458984375, 0.546875, 0.4033203125, 0.9169921875, 0.03515625, 0.7294921875, 0.1865234375, 0.3056640625,
		0.3974609375, 0.8818359375, 0.0283203125, 0.470703125, 0.9912109375, 0.7119140625, 0.548828125, 0.826171875, 0.0166015625, 0.6611328125, 0.392578125, 0.0947265625, 0.2939453125, 0.5791015625, 0.859375, 0.345703125, 0.9384765625, 0.20703125,
		0.5419921875, 0.7138671875, 0.115234375, 0.396484375, 0.58984375, 0.740234375, 0.1337890625, 0.8740234375, 0.080078125, 0.5908203125, 0.2275390625, 0.85546875, 0.63671875, 0.1181640625, 0.75, 0.521484375, 0.162109375, 0.78515625, 0.2890625,
		0.052734375, 0.1875, 0.328125, 0.765625, 0.4677734375, 0.984375, 0.7451171875, 0.4443359375, 0.2197265625, 0.515625, 0.0703125, 0.728515625, 0.4306640625, 0.857421875, 0.353515625, 0.2421875, 0.845703125, 0.0634765625, 0.2060546875, 0.6318359375,
		0.802734375, 0.486328125, 0.7705078125, 0.3486328125, 0.51171875, 0.4296875, 0.935546875, 0.2451171875, 0.6572265625, 0.912109375, 0.408203125, 0.5888671875, 0.861328125, 0.451171875, 0.9111328125, 0.580078125, 0.259765625, 0.1376953125,
		0.6083984375, 0.0390625, 0.666015625, 0.96484375, 0.166015625, 0.6357421875, 0.2734375, 0.087890625, 0.9931640625, 0.6171875, 0.7685546875, 0.51953125, 0.9208984375, 0.37109375, 0.2587890625, 0.685546875, 0.16796875, 0.99609375, 0.0859375,
		0.830078125, 0.0, 0.5556640625, 0.3271484375, 0.068359375, 0.2353515625, 0.72265625, 0.1220703125, 0.6435546875, 0.2255859375, 0.0732421875, 0.84375, 0.36328125, 0.892578125, 0.7783203125, 0.3359375, 0.8134765625, 0.3955078125, 0.8857421875,
		0.5576171875, 0.80078125, 0.48046875, 0.03125, 0.42578125, 0.2900390625, 0.697265625, 0.0107421875, 0.94921875 };


	if (randSeedX >= 688)
	{
		randSeedX = glm::linearRand(0u, 680u);
	}

	if (randSeedY >= 688)
	{
		randSeedY = glm::linearRand(0u, 680u);
	}

	vec2 res = vec2(BlueNoiseData[randSeedX++], BlueNoiseData[randSeedY++]);

	return res;
}
