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

void PathTracer::ParseParamsFromCommandLineArgs(PathTracer::Params& res, const char** args, int32_t num)
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

	float aspectRatio = (scene->HasCameras() && scene->mCameras[0]->mAspect > 0.0f)
		? scene->mCameras[0]->mAspect
		: (4.0f / 3.0f);

	const uint32_t height = params.m_height;
	const uint32_t width = static_cast<uint32_t>(height * aspectRatio);

	const float hFov = (scene->HasCameras() && scene->mCameras[0]->mHorizontalFOV > 0.0f)
		? scene->mCameras[0]->mHorizontalFOV
		: glm::radians(90.0f);

	const float vFov = (aspectRatio > 0.0f && hFov > 0.0f)
		? 2.0f * atan(tan(hFov / 2.0f) * (1.0f / aspectRatio))
		: 0.0f;

	const float zMin = scene->HasCameras() ? scene->mCameras[0]->mClipPlaneNear : 0.01f;
	const float zMax = scene->HasCameras() ? scene->mCameras[0]->mClipPlaneFar : 1000.0f;

	// Camera
	//auto cameraPos = vec3(0, 0, 10);
	auto cameraPos = vec3(-1.0f, 0.7f, -1.0f) * 0.5f;
	//auto cameraPos = glm::vec3(-2.8f, 2.7f, 5.5f) * 100.0f;

	auto cameraUp = normalize(vec3(0, 1, 0));
	auto cameraForward = normalize(-cameraPos);
	//auto cameraForward = glm::normalize(glm::vec3(0.0f, -0.0f, -1.0f));

	auto axis = normalize(cross(cameraForward, cameraUp));
	cameraUp = normalize(cross(axis, cameraForward));

	// View
	if (scene->HasCameras())
	{
		const auto& aiCamera = scene->mCameras[0];

		aiNode* pRootNode = scene->mRootNode;
		aiNode* pCameraNode = pRootNode->FindNode(aiCamera->mName);

		aiMatrix4x4 aiMatrix{};
		aiNode* cur = pCameraNode;
		while (cur != nullptr)
		{
			aiMatrix = aiMatrix * cur->mTransformation;
			cur = cur->mParent;
		}

		mat4 matrix{};
		::memcpy(&matrix, &aiMatrix, sizeof(float) * 16);
		::memcpy(&cameraUp, &aiCamera->mUp, sizeof(float) * 3);
		::memcpy(&cameraForward, &aiCamera->mLookAt, sizeof(float) * 3);
		::memcpy(&cameraPos, &aiCamera->mPosition, sizeof(float) * 3);

		const vec4 translation = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f) * matrix;
		cameraPos = vec3(translation.xyz) / translation.w;
		cameraUp = glm::normalize(glm::vec3(glm::vec4(cameraUp, 0.0f) * matrix));
		cameraForward = glm::normalize(glm::vec3(glm::vec4(cameraForward, 0.0f) * matrix));
	}

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

			if (aiMaterial->GetTexture(aiTextureType_TRANSMISSION, 0, &fileName) == AI_SUCCESS)
			{
				const std::string file(fileName.C_Str());
				if (m_textureMapping.ContainsKey(file))
				{
					material.m_transmissionIndex = m_textureMapping[file];
				}
				else
				{
					loadTexturesTasks.Emplace(LoadTexture_Task<vec3>(m_textures, params.m_pathToModel, scene, textureIndex, file, false));
					material.m_transmissionIndex = textureIndex;
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

#ifdef _DEBUG
						uint32_t debugX = 440;
						uint32_t debugY = 105;

						if (!(x < debugX && (x + GroupSize) > debugX &&
							y < debugY && (y + GroupSize) > debugY))
						{
							return;
						}
#endif
						for (uint32_t v = 0; v < GroupSize && (y + v) < height; v++)
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

		const LightingModel::SampledData sample = GetSampledData(tri.m_materialIndex, uv);
		const vec3 viewDirection = -normalize(ray.GetDirection());
		const vec3 worldNormal = normalize(tbn * sample.m_normal);

		RaycastHit hitLight{};
		const vec3 toLight = normalize(vec3(1, 1, -1));
		vec3 offset = 0.000001f * faceNormal;

		SAILOR_PROFILE_END_BLOCK();

		SAILOR_PROFILE_BLOCK("Direct lighting");
		Ray rayToLight(hit.m_point + offset, toLight);
		if (!bvh.IntersectBVH(rayToLight, hitLight, 0, std::numeric_limits<float>().max(), hit.m_triangleIndex))
		{
			const float angle = abs(glm::dot(toLight, worldNormal));
			res += LightingModel::CalculateBRDF(viewDirection, worldNormal, toLight, sample) * 3.0f * angle;
		}
		SAILOR_PROFILE_END_BLOCK();

		if (bounceLimit > 0)
		{
			SAILOR_PROFILE_BLOCK("Bounces");

			vec3 indirect = vec3(0.0f, 0.0f, 0.0f);

			const uint32_t numExtraSamples = bounceLimit == params.m_numBounces ? params.m_numSamples : 1;
			float contribution = 0.0f;

			for (uint32_t i = 0; i < numExtraSamples; i++)
			{
				const bool bFullMetallic = sample.m_orm.z == 1.0f && sample.m_orm.y == 0.0f;
				const bool bSpecular = bFullMetallic || glm::linearRand(0.0f, 1.0f) > 0.5f;

				const float importanceRoughness = bSpecular ? sample.m_orm.y : 1.0f;

				vec2 randomSample = vec2(glm::linearRand(0.0f, 1.0f), glm::linearRand(0.0f, 1.0f));
				vec3 H = LightingModel::ImportanceSampleGGX(randomSample, importanceRoughness, worldNormal);
				vec3 direction = 2.0f * dot(viewDirection, H) * H - viewDirection;

				const bool bHasTransmission = sample.m_orm.z < 1.0f && sample.m_transmission > 0.0f;
				const bool bTransmission = bHasTransmission && (glm::linearRand(0.0f, 1.0f) > 0.5f);

				if (bTransmission)
				{
					direction += 2.0f * worldNormal * dot(-direction, worldNormal);
				}

				const float pdfSpec = LightingModel::GGX_PDF(worldNormal, H, viewDirection, sample.m_orm.y);
				const float pdfLambert = abs(dot(direction, worldNormal)) / Math::Pi;

				float pdf = bFullMetallic ? pdfSpec : ((pdfSpec + pdfLambert) * 0.5f);

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

					const Ray rayToDirection(hit.m_point + offset, direction);
					indirect += glm::max(vec3(0.0f, 0.0f, 0.0f),
						(term *
							angle *
							Raytrace(rayToDirection, bvh, bounceLimit - 1, hit.m_triangleIndex, params)) / pdf);

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
		if (bounceLimit > 0 && sample.m_baseColor.a < 0.97f)
		{
			Math::Ray newRay{};
			newRay.SetDirection(ray.GetDirection());
			newRay.SetOrigin(hit.m_point + ray.GetDirection() * 0.0001f);

			PathTracer::Params p = params;
			p.m_numBounces = std::max(0u, params.m_numBounces - 1);

			res = res * sample.m_baseColor.a +
				Raytrace(newRay, bvh, bounceLimit - 1, hit.m_triangleIndex, p) * (1.0f - sample.m_baseColor.a);
		}
	}
	else
	{
		res = vec3(0.5f, 0.5f, 0.5f);
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

	return res;
}